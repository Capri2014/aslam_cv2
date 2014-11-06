#include <memory>
#include <vector>

#include <Eigen/Dense>
#include <glog/logging.h>

#include <aslam/cameras/camera.h>
#include <aslam/common/memory.h>
#define  ENABLE_STATISTICS 0
#include <aslam/common/statistics/statistics.h>
#include <aslam/frames/visual-frame.h>
#include <aslam/tracker/feature-tracker-gyro.h>

namespace aslam {

GyroTracker::GyroTracker(const std::shared_ptr<const aslam::Camera>& input_camera)
    : FeatureTracker(input_camera),
      previous_frame_ptr_(nullptr) {}

void GyroTracker::addFrame(std::shared_ptr<VisualFrame> current_frame_ptr,
                           const Eigen::Matrix3d& C_current_prev) {
  CHECK_NOTNULL(current_frame_ptr.get());
  CHECK(current_frame_ptr->hasKeypointMeasurements());

  // Initialize if: * this is the first frame
  //                * there are no keypoints in the current frame
  // keep this frame as the previous frame and return!
  if (previous_frame_ptr_.get() == nullptr ||
      current_frame_ptr->getKeypointMeasurements().cols() == 0) {
    // Initialize all keypoints as untracked.
    const size_t num_keypoints = current_frame_ptr->getKeypointMeasurements().cols();
    Eigen::VectorXi track_ids(num_keypoints);
    track_ids.fill(-1);
    current_frame_ptr->swapTrackIds(&track_ids);
    previous_track_lengths_.resize(num_keypoints, 0);
    previous_frame_ptr_ = current_frame_ptr;
    return;
  }

  // Convenience access references
  VisualFrame& current_frame = *current_frame_ptr;
  const VisualFrame& previous_frame = *CHECK_NOTNULL(previous_frame_ptr_.get());

  // Make sure the frames are in order time-wise
  // TODO(schneith): Maybe also enforce that deltaT < tolerance?
  CHECK_GT(current_frame.getHardwareTimestamp(), previous_frame.getHardwareTimestamp());

  // Check that the required data is available in the frame
  CHECK(current_frame.hasDescriptors());
  CHECK_EQ(current_frame.getDescriptors().rows(), current_frame.getDescriptorSizeBytes());
  CHECK_EQ(current_frame.getKeypointMeasurements().cols(), current_frame.getDescriptors().cols());

  // Match the keypoints in the current frame to the previous one.
  std::vector<std::pair<int, int> > matches_prev_current;
  matchFeatures(C_current_prev, current_frame, previous_frame, &matches_prev_current);

  aslam::statistics::DebugStatsCollector stats_num_matches("GyroTracker num. keypoint matches");
  stats_num_matches.AddSample(matches_prev_current.size());

  // Prepare buckets.
  std::vector<std::vector<int> > buckets;
  std::vector<std::pair<int, int> > candidates_new_tracks;
  candidates_new_tracks.reserve(matches_prev_current.size());
  buckets.resize(kNumberOfTrackingBuckets * kNumberOfTrackingBuckets);

  float bucket_width_x = static_cast<float>(camera_->imageWidth()) / kNumberOfTrackingBuckets;
  float bucket_width_y = static_cast<float>(camera_->imageHeight()) / kNumberOfTrackingBuckets;

  std::function<int(const Eigen::Block<Eigen::Matrix2Xd, 2, 1>&)> compute_bin_index =
      [buckets, bucket_width_x, bucket_width_y, this](const Eigen::Vector2d &kp) -> int {
        float bin_x = kp[0] / bucket_width_x;
        float bin_y = kp[1] / bucket_width_y;

        int bin_index = static_cast<int>(std::floor(bin_y)) * kNumberOfTrackingBuckets +
                        static_cast<int>(std::floor(bin_x));

        CHECK_GE(bin_index, 0);
        CHECK_LT(bin_index, static_cast<int>(buckets.size()));
        return bin_index;
      };

  const size_t current_num_pts = current_frame.getKeypointMeasurements().cols();
  Eigen::VectorXi current_track_ids(current_num_pts);
  current_track_ids.fill(-1); // Initialize as untracked.
  current_track_lengths_.clear();
  current_track_lengths_.resize(current_num_pts, 0);

  for (size_t i = 0; i < matches_prev_current.size(); ++i) {
    CHECK_LT(matches_prev_current[i].first, static_cast<int>(previous_frame.getTrackIds().rows()));
    CHECK_LT(matches_prev_current[i].second, static_cast<int>(current_track_ids.size()));
    current_track_ids(matches_prev_current[i].second) = previous_frame.getTrackId(
        matches_prev_current[i].first);
    current_track_lengths_[matches_prev_current[i].second] =
        previous_track_lengths_[matches_prev_current[i].first] + 1;

    // Check if this is a continued track.
    if (current_track_ids(matches_prev_current[i].second) >= 0) {
      // Put the current keypoint into the bucket.
      CHECK_LT(matches_prev_current[i].second, current_num_pts);
      const Eigen::Block<Eigen::Matrix2Xd, 2, 1>& keypoint = current_frame.getKeypointMeasurement(
          matches_prev_current[i].second);
      int bin_index = compute_bin_index(keypoint);
      buckets[bin_index].push_back(0);
      candidates_new_tracks.push_back(matches_prev_current[i]);
    }
  }

  LOG(WARNING) << "Got " << candidates_new_tracks.size() << " continued tracks";

  std::vector<std::pair<int, float> > candidates;
  candidates.reserve(matches_prev_current.size());
  for (size_t i = 0; i < matches_prev_current.size(); ++i) {
    int index_in_curr = matches_prev_current[i].second;
    const double& keypoint_score = current_frame.getKeypointScore(index_in_curr);
    aslam::statistics::DebugStatsCollector stats_laplacian_score("GyroTracker keypoint score");
    stats_laplacian_score.AddSample(keypoint_score);

    if (current_track_ids(index_in_curr) < 0) {
      candidates.emplace_back(i, keypoint_score);
    }
  }

  std::sort(candidates.begin(), candidates.end(),
            [](const std::pair<int, float> & lhs, const std::pair<int, float> & rhs) {
              return lhs.second < rhs.second;
            });

  // Unconditionally push the first very strong points.
  int candidate_idx = 0;
  for (; candidate_idx < std::min<int>(kNumberOfKeyPointsUseUnconditional, candidates.size());
      ++candidate_idx) {
    int match_idx = candidates[candidate_idx].first;
    int index_in_curr = matches_prev_current[match_idx].second;
    const Eigen::Block<Eigen::Matrix2Xd, 2, 1>& keypoint = current_frame.getKeypointMeasurement(
        index_in_curr);
    const double& keypoint_score = current_frame.getKeypointScore(index_in_curr);
    if (keypoint_score < kKeypointScoreThresholdUnconditional) {
      aslam::statistics::DebugStatsCollector stats_too_low_laplacian_score(
          "GyroTracker Too low laplacian score for unconditional");
      stats_too_low_laplacian_score.AddSample(keypoint_score);
      continue;
    }

    int bin_index = compute_bin_index(keypoint);
    buckets[bin_index].push_back(0);
    candidates_new_tracks.push_back(matches_prev_current[match_idx]);
    aslam::statistics::DebugStatsCollector stats_unconditionally(
        "GyroTracker Unconditionally accepted");
    stats_unconditionally.AddSample(keypoint_score);
  }

  int bucket_too_full = 0;
  // Now push as many strong points as there is space in the buckets.
  int num_pts_per_bucket = kNumberOfKeyPointsUseStrong / buckets.size();
  for (; candidate_idx < std::min<int>(kNumberOfKeyPointsUseStrong, candidates.size());
      ++candidate_idx) {
    int match_idx = candidates[candidate_idx].first;
    int index_in_curr = matches_prev_current[match_idx].second;
    const Eigen::Block<Eigen::Matrix2Xd, 2, 1>& keypoint = current_frame.getKeypointMeasurement(
        index_in_curr);
    const double& keypoint_score = current_frame.getKeypointScore(index_in_curr);
    if (keypoint_score < kKeypointScoreThresholdStrong) {
      aslam::statistics::DebugStatsCollector stats_too_low_keypoint_score_strong(
          "GyroTracker Too low score for strong");
      stats_too_low_keypoint_score_strong.AddSample(keypoint_score);
      continue;
    }
    int bin_index = compute_bin_index(keypoint);

    if (static_cast<int>(buckets[bin_index].size()) < num_pts_per_bucket) {
      buckets[bin_index].push_back(0);
      candidates_new_tracks.push_back(matches_prev_current[match_idx]);
      aslam::statistics::DebugStatsCollector stats_strong_acc("GyroTracker Strong accepted");
      stats_strong_acc.AddSample(keypoint_score);
    } else {
      ++bucket_too_full;
      aslam::statistics::DebugStatsCollector stats_bucket_too_full("GyroTracker Bucket too full");
      stats_bucket_too_full.AddSample(keypoint_score);
    }
  }

  // Assign new Id's to all candidates that are not continued tracks.
  aslam::statistics::DebugStatsCollector stats_track_length("GyroTracker Track lengths");
  aslam::VisualFrame::TrackIdsT* previous_track_ids = previous_frame.getTrackIdsMutable();
  CHECK_NOTNULL(previous_track_ids);
  int num_keypoints_in_previous_frame = previous_track_ids->rows();
  for (size_t i = 0; i < candidates_new_tracks.size(); ++i) {
    int index_current_frame = candidates_new_tracks[i].index_current_frame_;
    if (current_track_ids(index_current_frame) == -1) {
      int index_previous_frame = candidates_new_tracks[i].index_previous_frame_;
      CHECK_LT(index_previous_frame, num_keypoints_in_previous_frame);
      int previous_track_id = (*previous_track_ids)(index_previous_frame);
      CHECK_EQ(previous_track_id, -1) << "Have a match that supposedly represents a new track "
          "but the track id of the previous frame is not -1, so this would indicate a continued "
          "track, and not a new track!";

      int new_track_id = ++current_track_id_;
      current_track_ids(index_current_frame) = new_track_id;
      (*previous_track_ids)(index_previous_frame) = new_track_id;

      current_track_lengths_[index_current_frame] = 2;
    }
    stats_track_length.AddSample(current_track_lengths_[index_current_frame]);
  }

  // Save the output track-ids to the channel in the current frame.
  current_frame.swapTrackIds(&current_track_ids);

  // Keep the current track length and the current frame
  previous_track_lengths_.swap(current_track_lengths_);
  previous_frame_ptr_ = current_frame_ptr;

  // Print some statistics now and then.
  static int count = 0;
  if (count++ % 30 == 0) {
    LOG(WARNING) << statistics::Statistics::Print();
  }
}

void GyroTracker::matchFeatures(const Eigen::Matrix3d& C_current_prev,
                                const VisualFrame& current_frame,
                                const VisualFrame& previous_frame,
                                std::vector<std::pair<int, int> >* matches_prev_current) const {
  CHECK_NOTNULL(matches_prev_current);
  matches_prev_current->clear();

  struct KeypointAndIndex {
    EIGEN_MAKE_ALIGNED_OPERATOR_NEW
    Eigen::Vector2d measurement;
    int index;
  };

  // Sort keypoints by y-coordinate.
  const int current_num_pts = current_frame.getKeypointMeasurements().cols();
  Aligned<std::vector, KeypointAndIndex>::type current_keypoints_by_y;
  current_keypoints_by_y.resize(current_num_pts);

  for (int i = 0; i < current_num_pts; ++i) {
    current_keypoints_by_y[i].measurement = current_frame.getKeypointMeasurement(i),
    current_keypoints_by_y[i].index = i;
  }

  std::sort(current_keypoints_by_y.begin(), current_keypoints_by_y.end(),
            [](const KeypointAndIndex& lhs, const KeypointAndIndex& rhs)->bool {
              return lhs.measurement(1) < rhs.measurement(1);
            });

  // Build corner_row_LUT.
  std::vector<int> corner_row_LUT;
  const uint32_t image_height = camera_->imageHeight();
  corner_row_LUT.reserve(image_height);
  int v = 0;
  for (size_t y = 0; y < image_height; ++y) {
    while (v < current_num_pts && y > current_keypoints_by_y[v].measurement(1)) {
      ++v;
    }
    corner_row_LUT.push_back(v);
  }
  CHECK_EQ(static_cast<int>(corner_row_LUT.size()), image_height);

  // Undistort and predict previous keypoints.
  const int prev_num_pts =  previous_frame.getKeypointMeasurements().cols();

  Eigen::Matrix<double, 2, Eigen::Dynamic> C2_previous_image_points;
  C2_previous_image_points.resize(Eigen::NoChange, prev_num_pts);

  for (int i = 0; i < prev_num_pts; ++i) {
    // Backproject keypoint to bearing vector (remove distortion and projection effects)
    Eigen::Vector3d previous_bearing;
    const Eigen::Vector2d& previous_keypoint = previous_frame.getKeypointMeasurement(i);
    //TODO(schneith): use the vectorized functions
    camera_->backProject3(previous_keypoint, &previous_bearing);

    // Predict bearing direction using external data
    const Eigen::Vector3d bearing_predicted = C_current_prev * previous_bearing;

    // Project the predicted bearing vector back to the camera.
    Eigen::Vector2d predicted_keypoint;
    camera_->project3(bearing_predicted, &predicted_keypoint);

    C2_previous_image_points.block<2, 1>(0, i) = predicted_keypoint;
  }

  // Distance function.
  const unsigned int descriptorSizeBytes = current_frame.getDescriptorSizeBytes();
  CHECK_LT(descriptorSizeBytes * 8, 512);
  auto hammingDistance512 =
      [descriptorSizeBytes](const unsigned char* x, const unsigned char* y)->unsigned int {
        unsigned int distance = 0;
        for(unsigned int i = 0; i < descriptorSizeBytes; i++) {
          unsigned char val = *(x + i) ^ *(y + i);
          while(val) {
            ++distance;
            val &= val - 1;
          }
        }
        CHECK_LT(distance, descriptorSizeBytes * 8);
        return distance;
      };

  // Look for matches.
  static const int min_search_radius = 5;
  static const int search_radius = 10;

  matches_prev_current->reserve(prev_num_pts);

  for (int i = 0; i < prev_num_pts; ++i) {
    Eigen::Matrix<double, 2, 1> previous_predicted = C2_previous_image_points.block<2, 1>(0, i);
    CHECK_LT(i, prev_num_pts);
    CHECK_GE(i, 0);
    const unsigned char* previous_descriptor = previous_frame.getDescriptor(i);

    const int bound_left_nearest = previous_predicted(0) - min_search_radius;
    const int bound_right_nearest = previous_predicted(0) + min_search_radius;
    const int bound_left_near = previous_predicted(0) - search_radius;
    const int bound_right_near = previous_predicted(0) + search_radius;

    std::function<int(int, int, int)> clamp = [](int lower, int upper, int in) {
      return std::min<int>(std::max<int>(in, lower), upper);
    };

    // Get search area for LUT iterators (rowwise).
    int idxnearest[2];  // Min search region.
    idxnearest[0] = clamp(0, image_height - 1, previous_predicted(1) + 0.5 - min_search_radius);
    idxnearest[1] = clamp(0, image_height - 1, previous_predicted(1) + 0.5 + min_search_radius);
    int idxnear[2];  // Max search region.
    idxnear[0] = clamp(0, image_height - 1, previous_predicted(1) + 0.5 - search_radius);
    idxnear[1] = clamp(0, image_height - 1, previous_predicted(1) + 0.5 + search_radius);

    CHECK_LE(idxnearest[0], idxnearest[1]);
    CHECK_LE(idxnear[0], idxnear[1]);

    CHECK_GE(idxnearest[0], 0);
    CHECK_GE(idxnearest[1], 0);
    CHECK_GE(idxnear[0], 0);
    CHECK_GE(idxnear[1], 0);
    CHECK_LT(idxnearest[0], image_height);
    CHECK_LT(idxnearest[1], image_height);
    CHECK_LT(idxnear[0], image_height);
    CHECK_LT(idxnear[1], image_height);

    int nearest_top = std::min<int>(idxnearest[0], corner_row_LUT.size() - 1);
    int nearest_bottom = std::min<int>(idxnearest[1] + 1, corner_row_LUT.size() - 1);
    int near_top = std::min<int>(idxnear[0], corner_row_LUT.size() - 1);
    int near_bottom = std::min<int>(idxnear[1] + 1, corner_row_LUT.size() - 1);

    // Get corners in this area.
    typedef typename Aligned<std::vector, KeypointAndIndex>::type::const_iterator KeyPointIterator;
    KeyPointIterator nearest_corners_begin = current_keypoints_by_y.begin() + corner_row_LUT[nearest_top];
    KeyPointIterator nearest_corners_end = current_keypoints_by_y.begin() + corner_row_LUT[nearest_bottom];
    KeyPointIterator near_corners_begin = current_keypoints_by_y.begin() + corner_row_LUT[near_top];
    KeyPointIterator near_corners_end = current_keypoints_by_y.begin() + corner_row_LUT[near_bottom];

    // Get descriptors and match.
    static const int kMatchingThresholdBits = 120;
    bool found = false;
    int n_processed_corners = 0;
    KeyPointIterator it_best;
    int best_score = 512 - kMatchingThresholdBits;
    // Keep track of processed corners s.t. we don't process them again in the
    // large window.
    std::vector<uint8_t> processed_current_corners;
    processed_current_corners.resize(current_num_pts, false);

    // First search small window.
    for (KeyPointIterator it = nearest_corners_begin; it != nearest_corners_end; ++it) {
      if (it->measurement(0) < bound_left_nearest || it->measurement(0) > bound_right_nearest) {
        continue;
      }

      CHECK_LT(it->index, current_num_pts);
      CHECK_GE(it->index, 0);
      const unsigned char* const current_descriptor = current_frame.getDescriptor(it->index);
      int current_score = 512 - hammingDistance512(previous_descriptor, current_descriptor);
      if (current_score > best_score) {
        best_score = current_score;
        it_best = it;
        found = true;
        CHECK_LT((previous_predicted - it_best->measurement).norm(), min_search_radius * 2);

        aslam::statistics::DebugStatsCollector stats_distance_match("GyroTracker distance to match min");
        stats_distance_match.AddSample((previous_predicted - it_best->measurement).norm());
      }
      processed_current_corners[it->index] = true;
      ++n_processed_corners;
    }

    // If no match in small window, increase window and search again.
    if (!found) {
      for (KeyPointIterator it = near_corners_begin; it != near_corners_end; ++it) {
        if (processed_current_corners[it->index]) {
          continue;
        }
        if (it->measurement(0) < bound_left_near || it->measurement(0) > bound_right_near) {
          continue;
        }
        CHECK_LT(it->index, current_num_pts);
        CHECK_GE(it->index, 0);
        const unsigned char* const current_descriptor = current_frame.getDescriptor(it->index);
        int current_score = 512 - hammingDistance512(previous_descriptor, current_descriptor);
        if (current_score > best_score) {
          best_score = current_score;
          it_best = it;
          found = true;
          CHECK_LT((previous_predicted - it_best->measurement).norm(), search_radius * 2);

          aslam::statistics::DebugStatsCollector stats_distance_match("GyroTracker distance to match norm");
          stats_distance_match.AddSample((previous_predicted - it_best->measurement).norm());
        }
        processed_current_corners[it->index] = true;
        ++n_processed_corners;
      }
    }

    if (found) {
      matches_prev_current->emplace_back(i, it_best->index);
      aslam::statistics::DebugStatsCollector stats_distance_match("GyroTracker match bits");
      stats_distance_match.AddSample(best_score);
    } else {
      aslam::statistics::DebugStatsCollector stats_distance_no_match("GyroTracker no-match num_checked");
      stats_distance_no_match.AddSample(n_processed_corners);
    }
  }
}

}  //namespace aslam
