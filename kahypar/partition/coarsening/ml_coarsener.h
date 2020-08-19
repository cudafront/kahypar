/*******************************************************************************
 * This file is part of KaHyPar.
 *
 * Copyright (C) 2016 Sebastian Schlag <sebastian.schlag@kit.edu>
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
#include <string>
#include <vector>


#include "kahypar/definitions.h"
#include "kahypar/macros.h"
#include "kahypar/meta/mandatory.h"
#include "kahypar/partition/coarsening/i_coarsener.h"
#include "kahypar/partition/coarsening/vertex_pair_coarsener_base.h"
#include "kahypar/partition/coarsening/policies/fixed_vertex_acceptance_policy.h"
#include "kahypar/partition/coarsening/policies/rating_acceptance_policy.h"
#include "kahypar/partition/coarsening/policies/rating_community_policy.h"
#include "kahypar/partition/coarsening/policies/rating_heavy_node_penalty_policy.h"
#include "kahypar/partition/coarsening/policies/rating_partition_policy.h"
#include "kahypar/partition/coarsening/policies/rating_score_policy.h"
#include "kahypar/partition/coarsening/policies/rating_tie_breaking_policy.h"
#include "kahypar/partition/coarsening/vertex_pair_rater.h"

namespace kahypar {
template <class ScorePolicy = HeavyEdgeScore,
          class HeavyNodePenaltyPolicy = NoWeightPenalty,
          class CommunityPolicy = UseCommunityStructure,
          class RatingPartitionPolicy = NormalPartitionPolicy,
          class AcceptancePolicy = BestRatingPreferringUnmatched<>,
          class FixedVertexPolicy = AllowFreeOnFixedFreeOnFreeFixedOnFixed,
          typename RatingType = RatingType,
          typename ContractionFunc = Mandatory,
          typename EndOfPassFunc = Mandatory,
          typename UncontractFunc = Mandatory>
class MLCoarsener final : public ICoarsener,
                          private VertexPairCoarsenerBase<ds::BinaryMaxHeap<HypernodeID, RatingType>,
                                                          UncontractFunc>{
 private:
  static constexpr bool debug = false;

  static constexpr HypernodeID kInvalidTarget = std::numeric_limits<HypernodeID>::max();

  using Base = VertexPairCoarsenerBase<ds::BinaryMaxHeap<HypernodeID, RatingType>,
                                       UncontractFunc>;
  using Rater = VertexPairRater<ScorePolicy,
                                HeavyNodePenaltyPolicy,
                                CommunityPolicy,
                                RatingPartitionPolicy,
                                AcceptancePolicy,
                                FixedVertexPolicy,
                                RatingType>;
  using Rating = typename Rater::Rating;
  using Memento = typename Hypergraph::Memento;

 public:
  MLCoarsener(Hypergraph& hypergraph, const Context& context,
              const HypernodeWeight weight_of_heaviest_node,
              const ContractionFunc contraction_func,
              const EndOfPassFunc end_of_pass_func,
              const UncontractFunc uncontract_func) :
    Base(hypergraph, context, weight_of_heaviest_node, uncontract_func),
    _rater(_hg, _context),
    _contraction_func(contraction_func),
    _end_of_pass_func(end_of_pass_func) { }

  ~MLCoarsener() override = default;

  MLCoarsener(const MLCoarsener&) = delete;
  MLCoarsener& operator= (const MLCoarsener&) = delete;

  MLCoarsener(MLCoarsener&&) = delete;
  MLCoarsener& operator= (MLCoarsener&&) = delete;

  void setUncontractionFunction(const UncontractFunc uncontraction_func) {
    _uncontraction_func = uncontraction_func;
  }

  void simulateContractions(const std::vector<Memento>& mementos) {
    for ( const Memento& memento : mementos ) {
      ASSERT(_hg.nodeIsEnabled(memento.u));
      ASSERT(_hg.nodeIsEnabled(memento.v));
      Base::performContraction(memento.u, memento.v);
      _contraction_func(memento.u, memento.v);
    }
  }

 private:
  void coarsenImpl(const HypernodeID limit) override final {
    int pass_nr = 0;
    std::vector<HypernodeID> current_hns;
    while (_hg.currentNumNodes() > limit) {
      DBG << V(pass_nr);
      DBG << V(_hg.currentNumNodes());
      DBG << V(_hg.currentNumEdges());
      _rater.resetMatches();
      current_hns.clear();
      const HypernodeID num_hns_before_pass = _hg.currentNumNodes();
      for (const HypernodeID& hn : _hg.nodes()) {
        current_hns.push_back(hn);
      }
      Randomize::instance().shuffleVector(current_hns, current_hns.size());

      // std::sort(std::begin(current_hns), std::end(current_hns),
      //           [&](const HypernodeID l, const HypernodeID r) {
      //             return _hg.nodeDegree(l) < _hg.nodeDegree(r);
      //           });

      for (const HypernodeID& hn : current_hns) {
        if (_hg.nodeIsEnabled(hn)) {
          const Rating rating = _rater.rate(hn);

          if (rating.target != kInvalidTarget) {
            _rater.markAsMatched(hn);
            _rater.markAsMatched(rating.target);
            // if (_hg.nodeDegree(hn) > _hg.nodeDegree(rating.target)) {

            Base::performContraction(hn, rating.target);
            _contraction_func(hn, rating.target);
            // } else {
            //   contract(rating.target, hn);
            // }
          }

          if (_hg.currentNumNodes() <= limit) {
            break;
          }
        }
      }

      _end_of_pass_func();

      if (num_hns_before_pass == _hg.currentNumNodes()) {
        break;
      }
      ++pass_nr;
    }
  }

  bool uncoarsenImpl(IRefiner& refiner) override final {
    return Base::doUncoarsen(refiner);
  }

  using Base::_pq;
  using Base::_hg;
  using Base::_context;
  using Base::_history;
  Rater _rater;
  const ContractionFunc _contraction_func;
  const EndOfPassFunc _end_of_pass_func;
  using Base::_uncontraction_func;
};
}  // namespace kahypar
