#ifndef TRIANGULATION_H_
#define TRIANGULATION_H_
#include <vector>

#include <aslam/common/memory.h>
#include <aslam/common/pose-types.h>
#include <Eigen/Dense>
#include <Eigen/QR>
#include <glog/logging.h>

namespace aslam {

/// \brief This struct is returned by the triangulator and holds the result state
///        of the triangulation operation.
struct TriangulationResult {
  /// Possible projection state.
  enum class Status {
    /// The triangulation was successful.
    SUCCESSFUL,
    /// There were too few (< 2) landmark observations.
    TOO_FEW_MEASUREMENTS,
    /// The landmark is not fully observable (rank deficiency).
    UNOBSERVABLE,
    /// Default value after construction.
    UNINITIALIZED
  };
  // Make the enum values accessible from the outside without the additional indirection.
  static Status SUCCESSFUL;
  static Status TOO_FEW_MEASUREMENTS;
  static Status UNOBSERVABLE;
  static Status UNINITIALIZED;

  constexpr TriangulationResult() : status_(Status::UNINITIALIZED) {};
  constexpr TriangulationResult(Status status) : status_(status) {};

  /// \brief The triangulation result can be typecasted to bool and is true if the triangulation
  ///        is successful.
  explicit operator bool() const { return wasTriangulationSuccessful(); };

  /// \brief Compare objects.
  bool operator==(const TriangulationResult& other) const { return status_ == other.status_; };

  /// \brief Compare triangulation status.
  bool operator==(const TriangulationResult::Status& other) const { return status_ == other; };

  /// \brief Convenience function to print the state using streams.
  friend std::ostream& operator<< (std::ostream& out, const TriangulationResult& state)
  {
    std::string enum_str;
    switch (state.status_){
      case Status::SUCCESSFUL:                enum_str = "SUCCESSFUL"; break;
      case Status::TOO_FEW_MEASUREMENTS:      enum_str = "TOO_FEW_MEASUREMENTS"; break;
      case Status::UNOBSERVABLE:              enum_str = "UNOBSERVABLE"; break;
      default:
        case Status::UNINITIALIZED:             enum_str = "UNINITIALIZED"; break;
    }
    out << "ProjectionResult: " << enum_str << std::endl;
    return out;
  }

  /// \brief Check whether the triangulation was successful.
  bool wasTriangulationSuccessful() const { return (status_ == Status::SUCCESSFUL); };

  /// \brief Returns the exact state of the triangulation operation.
  Status getDetailedStatus() const { return status_; };

 private:
  /// Stores the projection state.
  Status status_;
};

/// brief Triangulate a 3d point from a set of n keypoint measurements on the
///       normalized camera plane.
/// @param measurements_normalized Keypoint measurements on normalized camera
///       plane.
/// @param T_W_B Pose of the body frame of reference w.r.t. the global frame,
///       expressed in the global frame.
/// @param T_B_C Pose of the camera w.r.t. the body frame expressed in the body
///       frame of reference.
/// @param G_point Triangulated point in global frame.
/// @return Was the triangulation successful?
inline TriangulationResult linearTriangulateFromNViews(
    const Aligned<std::vector, Eigen::Vector2d>::type& measurements_normalized,
    const Aligned<std::vector, aslam::Transformation>::type& T_G_B,
    const aslam::Transformation& T_B_C, Eigen::Vector3d* G_point) {
  CHECK_NOTNULL(G_point);
  CHECK_EQ(measurements_normalized.size(), T_G_B.size());
  if (measurements_normalized.size() < 2u) {
    return TriangulationResult(TriangulationResult::TOO_FEW_MEASUREMENTS);
  }

  const size_t rows = 3 * measurements_normalized.size();
  const size_t cols = 3 + measurements_normalized.size();
  Eigen::MatrixXd A = Eigen::MatrixXd::Zero(rows, cols);
  Eigen::VectorXd b = Eigen::VectorXd::Zero(rows);

  const Eigen::Matrix3d R_B_C = T_B_C.getRotationMatrix();

  // Fill in A and b.
  for (size_t i = 0; i < measurements_normalized.size(); ++i) {
    Eigen::Vector3d v(measurements_normalized[i](0),
        measurements_normalized[i](1), 1.);
    Eigen::Matrix3d R_G_B = T_G_B[i].getRotationMatrix();
    const Eigen::Vector3d& p_G_B = T_G_B[i].getPosition();
    A.block<3, 3>(3 * i, 0) = Eigen::Matrix3d::Identity();
    A.block<3, 1>(3 * i, 3 + i) = -R_G_B * R_B_C * v;
    b.segment<3>(3 * i) = p_G_B + R_G_B * T_B_C.getPosition();
  }

  Eigen::ColPivHouseholderQR<Eigen::MatrixXd> qr = A.colPivHouseholderQr();
  static constexpr double kRankLossTolerance = 0.001;
  qr.setThreshold(kRankLossTolerance);
  const size_t rank = qr.rank();
  if ((rank - measurements_normalized.size()) < 3) {
    return TriangulationResult(TriangulationResult::UNOBSERVABLE);
  }

  *G_point = qr.solve(b).head<3>();
  return TriangulationResult(TriangulationResult::SUCCESSFUL);
}

/// brief Triangulate a 3d point from a set of n keypoint measurements in
///       m cameras.
/// @param measurements_normalized Keypoint measurements on normalized image
///        plane. Should be n long.
/// @param measurement_camera_indices Which camera index each measurement
///        corresponds to. Should be n long, and should be 0 <= index < m.
/// @param T_W_B Pose of the body frame of reference w.r.t. the global frame,
///        expressed in the global frame. Should be n long.
/// @param T_B_C Pose of the cameras w.r.t. the body frame expressed in the body
///        frame of reference. Should be m long.
/// @param G_point Triangulated point in global frame.
/// @return Was the triangulation successful?
inline bool linearTriangulateFromNViewsMultiCam(
    const Aligned<std::vector, Eigen::Vector2d>::type& measurements_normalized,
    const std::vector<int>& measurement_camera_indices,
    const Aligned<std::vector, aslam::Transformation>::type& T_G_B,
    const Aligned<std::vector, aslam::Transformation>::type& T_B_C,
    Eigen::Vector3d* G_point) {
  CHECK_NOTNULL(G_point);
  CHECK_EQ(measurements_normalized.size(), T_G_B.size());
  CHECK_EQ(measurements_normalized.size(), measurement_camera_indices.size());
  if (measurements_normalized.size() < 2u) {
    return false;
  }

  const size_t rows = 3 * measurements_normalized.size();
  const size_t cols = 3 + measurements_normalized.size();
  Eigen::MatrixXd A = Eigen::MatrixXd::Zero(rows, cols);
  Eigen::VectorXd b = Eigen::VectorXd::Zero(rows);

  // Fill in A and b.
  for (size_t i = 0; i < measurements_normalized.size(); ++i) {
    int cam_index = measurement_camera_indices[i];
    CHECK_LT(cam_index, T_B_C.size());
    Eigen::Vector3d v(measurements_normalized[i](0),
        measurements_normalized[i](1), 1.);
    const Eigen::Vector3d& t_B_C = T_B_C[cam_index].getPosition();

    A.block<3, 3>(3 * i, 0) = Eigen::Matrix3d::Identity();
    A.block<3, 1>(3 * i, 3 + i) = -1.0 * T_G_B[i].getRotation().rotate(
        T_B_C[cam_index].getRotation().rotate(v));
    b.segment<3>(3 * i) = T_G_B[i] * t_B_C;
  }

  Eigen::ColPivHouseholderQR<Eigen::MatrixXd> qr = A.colPivHouseholderQr();
  static constexpr double kRankLossTolerance = 0.001;
  qr.setThreshold(kRankLossTolerance);
  const size_t rank = qr.rank();
  if ((rank - measurements_normalized.size()) < 3) {
    return false;
  }

  *G_point = qr.solve(b).head<3>();
  return true;
}

}  // namespace aslam
#endif  // TRIANGULATION_H_
