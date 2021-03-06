/*
 *  MEGAHIT
 *  Copyright (C) 2014 - 2015 The University of Hong Kong
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/* contact: Dinghua Li <dhli@cs.hku.hk> */

#include "assembly_algorithms.h"
#include <assert.h>
#include <stdio.h>
#include <omp.h>
#include <assert.h>
#include <vector>
#include <map>
#include <algorithm>
#include <unordered_set>
#include <parallel/algorithm>

#include "branch_group.h"
#include "atomic_bit_vector.h"
#include "unitig_graph.h"
#include "utils.h"

using std::vector;
using std::string;
using std::map;

namespace assembly_algorithms {

static AtomicBitVector marked;
static inline void MarkNode(SuccinctDBG &dbg, int64_t node_idx);

int64_t NextSimplePathNode(SuccinctDBG &dbg, int64_t cur_node) {
    int64_t next_node = dbg.UniqueOutgoing(cur_node);
    if (next_node != -1 && dbg.UniqueIncoming(next_node) != -1) {
        return next_node;
    } else {
        return -1;
    }
}

int64_t PrevSimplePathNode(SuccinctDBG &dbg, int64_t cur_node) {
    int64_t prev_node = dbg.UniqueIncoming(cur_node);
    if (prev_node != -1 && dbg.UniqueOutgoing(prev_node) != -1) {
        return prev_node;
    } else {
        return -1;
    }
}

int64_t Trim(SuccinctDBG &dbg, int len, int min_final_standalone) {
    int64_t number_tips = 0;
    omp_lock_t path_lock;
    omp_init_lock(&path_lock);
    marked.reset(dbg.size);

    #pragma omp parallel for reduction(+:number_tips)
    for (int64_t node_idx = 0; node_idx < dbg.size; ++node_idx) {
        if (dbg.IsValidNode(node_idx) && !marked.get(node_idx) && dbg.IsLast(node_idx) && dbg.OutdegreeZero(node_idx)) {
            vector<int64_t> path = {node_idx};
            int64_t prev_node;
            int64_t cur_node = node_idx;
            bool is_tip = false;
            for (int i = 1; i < len; ++i) {
                prev_node = dbg.UniqueIncoming(cur_node);
                if (prev_node == -1) {
                    is_tip = dbg.IndegreeZero(cur_node); // && (i + dbg.kmer_k - 1 < min_final_standalone);
                    break;
                } else if (dbg.UniqueOutgoing(prev_node) == -1) {
                    is_tip = true;
                    break;
                } else {
                    path.push_back(prev_node);
                    cur_node = prev_node;
                }
            }

            if (is_tip) {
                for (unsigned i = 0; i < path.size(); ++i) {
                    MarkNode(dbg, path[i]);
                }
                ++number_tips;
            }
        }
    }

    #pragma omp parallel for reduction(+:number_tips)
    for (int64_t node_idx = 0; node_idx < dbg.size; ++node_idx) {
        if (dbg.IsValidNode(node_idx) && dbg.IsLast(node_idx) && !marked.get(node_idx) && dbg.IndegreeZero(node_idx)) {
            vector<int64_t> path = {node_idx};
            int64_t next_node;
            int64_t cur_node = node_idx;
            bool is_tip = false;
            for (int i = 1; i < len; ++i) {
                next_node = dbg.UniqueOutgoing(cur_node);
                if (next_node == -1) {
                    is_tip = dbg.OutdegreeZero(cur_node); // && (i + dbg.kmer_k - 1 < min_final_standalone);
                    break;
                } else if (dbg.UniqueIncoming(next_node) == -1) {
                    is_tip = true;
                } else {
                    path.push_back(next_node);
                    cur_node = next_node;
                }
            }

            if (is_tip) {
                for (unsigned i = 0; i < path.size(); ++i) {
                    MarkNode(dbg, path[i]);
                }
                ++number_tips;
            }
        }
    }

    #pragma omp parallel for
    for (int64_t node_idx = 0; node_idx < dbg.size; ++node_idx) {
        if (marked.get(node_idx)) {
            dbg.SetInvalid(node_idx);
        }
    }

    return number_tips;
}

int64_t RemoveTips(SuccinctDBG &dbg, int max_tip_len, int min_final_standalone) {
    int64_t number_tips = 0;
    xtimer_t timer;
    for (int len = 2; len < max_tip_len; len *= 2) {
        xlog("Removing tips with length less than %d; ", len);
        timer.reset();
        timer.start();
        number_tips += Trim(dbg, len, min_final_standalone);
        timer.stop();
        xlog_ext("Accumulated tips removed: %lld; time elapsed: %.4f\n", (long long)number_tips, timer.elapsed());
    }
    xlog("Removing tips with length less than %d; ", max_tip_len);
    timer.reset();
    timer.start();
    number_tips += Trim(dbg, max_tip_len, min_final_standalone);
    timer.stop();
    xlog_ext("Accumulated tips removed: %lld; time elapsed: %.4f\n", (long long)number_tips, timer.elapsed());

    {
        AtomicBitVector empty;
        marked.swap(empty);
    }

    return number_tips;
}

int64_t PopBubbles(SuccinctDBG &dbg, int max_bubble_len, double low_depth_ratio) {
    omp_lock_t bubble_lock;
    omp_init_lock(&bubble_lock);
    const int kMaxBranchesPerGroup = 4;
    if (max_bubble_len <= 0) {
        max_bubble_len = dbg.kmer_k * 2 + 2;
    }
    vector<std::pair<int, int64_t> > bubble_candidates;
    int64_t num_bubbles = 0;

    #pragma omp parallel for
    for (int64_t node_idx = 0; node_idx < dbg.size; ++node_idx) {
        if (dbg.IsValidNode(node_idx) && dbg.IsLast(node_idx) && dbg.Outdegree(node_idx) > 1) {
            BranchGroup bubble(&dbg, node_idx, kMaxBranchesPerGroup, max_bubble_len);
            if (bubble.Search()) {
                omp_set_lock(&bubble_lock);
                bubble_candidates.push_back(std::make_pair(bubble.length(), node_idx));
                omp_unset_lock(&bubble_lock);
            }
        }
    }

    for (unsigned i = 0; i < bubble_candidates.size(); ++i) {
        BranchGroup bubble(&dbg, bubble_candidates[i].second, kMaxBranchesPerGroup, max_bubble_len);
        if (bubble.Search() && bubble.RemoveErrorBranches(low_depth_ratio)) {
            ++num_bubbles;
        }
    }

    omp_destroy_lock(&bubble_lock);
    return num_bubbles;
}

static inline void MarkNode(SuccinctDBG &dbg, int64_t node_idx) {
    node_idx = dbg.GetLastIndex(node_idx);
    marked.set(node_idx);
}

} // namespace assembly_algorithms