/*******************************************************************************
 * This file is part of KaHyPar.
 *
 * Copyright (C) 2014-2016 Sebastian Schlag <sebastian.schlag@kit.edu>
 *
 * KaHyPar is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * KaHyPar is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with KaHyPar.  If not, see <http://www.gnu.org/licenses/>.
 *
 ******************************************************************************/

#pragma once

#include <limits>
#include <stack>
#include <vector>

#include "kahypar/datastructure/fast_reset_flag_array.h"
#include "kahypar/datastructure/sparse_map.h"
#include "kahypar/definitions.h"
#include "kahypar/macros.h"
#include "kahypar/partition/coarsening/policies/rating_acceptance_policy.h"
#include "kahypar/partition/coarsening/policies/rating_heavy_node_penalty_policy.h"
#include "kahypar/partition/context.h"
#include "kahypar/partition/preprocessing/louvain.h"
#include "kahypar/utils/stats.h"

namespace kahypar {
template <typename _RatingType,
          class _TieBreakingPolicy,
          class _AcceptancePolicy = BestRatingWithRandomTieBreaking<_TieBreakingPolicy>,
          class _NodeWeightPenalty = MultiplicativePenalty>
class HeavyEdgeRater {
 private:
  static constexpr bool debug = false;
  using AcceptancePolicy = _AcceptancePolicy;
  using NodeWeightPenalty = _NodeWeightPenalty;

  class HeavyEdgeRating {
 public:
    HeavyEdgeRating(HypernodeID trgt, RatingType val, bool is_valid) :
      target(trgt),
      value(val),
      valid(is_valid) { }

    HeavyEdgeRating() :
      target(std::numeric_limits<HypernodeID>::max()),
      value(std::numeric_limits<RatingType>::min()),
      valid(false) { }

    HeavyEdgeRating(const HeavyEdgeRating&) = delete;
    HeavyEdgeRating& operator= (const HeavyEdgeRating&) = delete;

    HeavyEdgeRating(HeavyEdgeRating&&) = default;
    HeavyEdgeRating& operator= (HeavyEdgeRating&&) = delete;

    ~HeavyEdgeRating() = default;

    HypernodeID target;
    RatingType value;
    bool valid;
  };

 public:
  using RatingType = _RatingType;
  using Rating = HeavyEdgeRating;

  HeavyEdgeRater(Hypergraph& hypergraph, const Context& context) :
    _hg(hypergraph),
    _context(context),
    _tmp_ratings(_hg.initialNumNodes()),
    _comm(),
    _already_matched(_hg.initialNumNodes()) {
    if (_context.preprocessing.enable_louvain_community_detection) {
      const bool verbose_output = (_context.type == ContextType::main &&
                                   _context.partition.verbose_output);
      if (verbose_output) {
        LOG << "Performing community detection:";
      }
      _comm = detectCommunities(_hg, _context);
      if (verbose_output) {
        LOG << "  # communities = " << context.stats->preprocessing("Communities");
        LOG << "  modularity    = " << context.stats->preprocessing("Modularity");
      }
    } else {
      _comm.resize(_hg.initialNumNodes(), 0);
    }
  }

  HeavyEdgeRater(const HeavyEdgeRater&) = delete;
  HeavyEdgeRater& operator= (const HeavyEdgeRater&) = delete;

  HeavyEdgeRater(HeavyEdgeRater&&) = delete;
  HeavyEdgeRater& operator= (HeavyEdgeRater&&) = delete;

  ~HeavyEdgeRater() = default;

  HeavyEdgeRating rate(const HypernodeID u) {
    DBG << "Calculating rating for HN" << u;
    const HypernodeWeight weight_u = _hg.nodeWeight(u);
    const PartitionID part_u = _hg.partID(u);
    for (const HyperedgeID& he : _hg.incidentEdges(u)) {
      ASSERT(_hg.edgeSize(he) > 1, V(he));
      const RatingType score = static_cast<RatingType>(_hg.edgeWeight(he)) / (_hg.edgeSize(he) - 1);
      for (const HypernodeID& v : _hg.pins(he)) {
        if (v != u && belowThresholdNodeWeight(weight_u, _hg.nodeWeight(v)) &&
            (part_u == _hg.partID(v))) {
          _tmp_ratings[v] += score;
        }
      }
    }

    RatingType max_rating = std::numeric_limits<RatingType>::min();
    HypernodeID target = std::numeric_limits<HypernodeID>::max();
    for (auto it = _tmp_ratings.end() - 1; it >= _tmp_ratings.begin(); --it) {
      const HypernodeID tmp_target = it->key;
      const RatingType tmp_rating = it->value /
                                    NodeWeightPenalty::penalty(weight_u,
                                                               _hg.nodeWeight(tmp_target));
      DBG << "r(" << u << "," << tmp_target << ")=" << tmp_rating;
      if (_comm[u] == _comm[tmp_target] &&
          AcceptancePolicy::acceptRating(tmp_rating, max_rating,
                                         target, tmp_target, _already_matched)) {
        max_rating = tmp_rating;
        target = tmp_target;
      }
    }

    _tmp_ratings.clear();
    HeavyEdgeRating ret;
    if (max_rating != std::numeric_limits<RatingType>::min()) {
      ASSERT(target != std::numeric_limits<HypernodeID>::max(), "invalid contraction target");
      ret.value = max_rating;
      ret.target = target;
      ret.valid = true;
      ASSERT(_comm[u] == _comm[ret.target]);
    }
    ASSERT([&]() {
        bool flag = true;
        if (ret.valid && (_hg.partID(u) != _hg.partID(ret.target))) {
          flag = false;
        }
        return flag;
      } (), "Representative" << u << "& contraction target" << ret.target
                             << "are in different parts!");
    DBG << "rating=(" << ret.value << "," << ret.target << "," << ret.valid << ")";
    return ret;
  }

  void markAsMatched(const HypernodeID hn) {
    _already_matched.set(hn, true);
  }

  void resetMatches() {
    _already_matched.reset();
  }

  HypernodeWeight thresholdNodeWeight() const {
    return _context.coarsening.max_allowed_node_weight;
  }

 private:
  bool belowThresholdNodeWeight(const HypernodeWeight weight_u,
                                const HypernodeWeight weight_v) const {
    return weight_v + weight_u <= _context.coarsening.max_allowed_node_weight;
  }

  Hypergraph& _hg;
  const Context& _context;
  ds::SparseMap<HypernodeID, RatingType> _tmp_ratings;
  std::vector<ClusterID> _comm;
  ds::FastResetFlagArray<> _already_matched;
};
}  // namespace kahypar
