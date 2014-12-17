#ifndef ASLAM_CV_MATCHINGENGINE_NON_EXCLUSIVE_H_
#define ASLAM_CV_MATCHINGENGINE_NON_EXCLUSIVE_H_

#include <glog/logging.h>
#include <vector>

#include <aslam/common/macros.h>
#include <aslam/matcher/match.h>

#include "aslam/matcher/matching-engine.h"

/// \addtogroup Matching
/// @{
///
/// @}

namespace aslam {

/// \brief Matching engine to simply return the best apple for each banana.
///        This explicitly does not deal with bananas matching to multiple apples and vice versa.
template<typename MatchingProblem>
class MatchingEngineNonExclusive : public MatchingEngine<MatchingProblem> {
 public:
  ASLAM_POINTER_TYPEDEFS(MatchingEngineNonExclusive);
  ASLAM_DISALLOW_EVIL_CONSTRUCTORS(MatchingEngineNonExclusive);

  MatchingEngineNonExclusive() {};
  virtual ~MatchingEngineNonExclusive() {};
  virtual bool match(MatchingProblem* problem, typename aslam::Matches* matches);
};

template<typename MatchingProblem>
bool MatchingEngineNonExclusive<MatchingProblem>::match(MatchingProblem* problem,
                                                        aslam::Matches* matches) {
  CHECK_NOTNULL(problem);
  CHECK_NOTNULL(matches);
  matches->clear();

  if (problem->doSetup()) {
    size_t num_bananas = problem->numBananas();

    for (size_t index_banana = 0; index_banana < num_bananas; ++index_banana) {
      typename MatchingProblem::Candidates candidates;

      problem->getAppleCandidatesForBanana(index_banana, &candidates);

      auto best_candidate = candidates.begin();
      for (auto it = candidates.begin(); it != candidates.end(); ++it) {
        if (it->score > best_candidate->score) best_candidate = it;
      }

      if (best_candidate != candidates.end()) {
        matches->emplace_back(best_candidate->index_apple, index_banana, best_candidate->score);
      }
    }
    VLOG(10) << "Matched " << matches->size() << " keypoints.";
    return true;
  } else {
    LOG(ERROR) << "Setting up the matching problem (.doSetup()) failed.";
    return false;
  }
}

}  // namespace aslam
#endif //ASLAM_CV_MATCHINGENGINE_NON_EXCLUSIVE_H_