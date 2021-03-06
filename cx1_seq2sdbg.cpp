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


#include "cx1_seq2sdbg.h"

#include <omp.h>
#include <string>
#include <vector>

#include "utils.h"
#include "packed_reads.h"
#include "mac_pthread_barrier.h"
#include "kmer.h"

#ifndef USE_GPU
#include "lv2_cpu_sort.h"
#else
#include "lv2_gpu_functions.h"
#endif

namespace cx1_seq2sdbg {

// helpers
typedef CX1<seq2sdbg_global_t, kNumBuckets> cx1_t;
typedef CX1<seq2sdbg_global_t, kNumBuckets>::readpartition_data_t readpartition_data_t;
typedef CX1<seq2sdbg_global_t, kNumBuckets>::bucketpartition_data_t bucketpartition_data_t;
typedef CX1<seq2sdbg_global_t, kNumBuckets>::outputpartition_data_t outputpartition_data_t;

/**
 * @brief encode seq_id and its offset in one int64_t
 */
inline int64_t EncodeEdgeOffset(int64_t seq_id, int offset, int strand, SequencePackage &p) {
    return ((p.get_start_index(seq_id) + offset) << 1) | strand;
}

inline bool IsDiffKMinusOneMer(uint32_t *item1, uint32_t *item2, int64_t spacing, int kmer_k) {
    // mask extra bits
    int chars_in_last_word = (kmer_k - 1) % kCharsPerEdgeWord;
    int num_full_words = (kmer_k - 1) / kCharsPerEdgeWord;
    if (chars_in_last_word > 0) {
        uint32_t w1 = item1[num_full_words * spacing];
        uint32_t w2 = item2[num_full_words * spacing];
        if ((w1 >> (kCharsPerEdgeWord - chars_in_last_word) * kBitsPerEdgeChar) != (w2 >> (kCharsPerEdgeWord - chars_in_last_word) * kBitsPerEdgeChar)) {
            return true;
        }
    }

    for (int i = num_full_words - 1; i >= 0; --i) {
        if (item1[i * spacing] != item2[i * spacing]) {
            return true;
        }
    }
    return false;
}

inline int ExtractFirstChar(uint32_t *item) {
    return *item >> kTopCharShift;
}

inline int Extract_a(uint32_t *item, int num_words, int64_t spacing, int kmer_k) {
    int non_dollar = (item[(num_words - 1) * spacing] >> (kBWTCharNumBits + kBitsPerMulti_t)) & 1;
    if (non_dollar) {
        int which_word = (kmer_k - 1) / kCharsPerEdgeWord;
        int word_index = (kmer_k - 1) % kCharsPerEdgeWord;
        return (item[which_word * spacing] >> (kCharsPerEdgeWord - 1 - word_index) * kBitsPerEdgeChar) & kEdgeCharMask;
    } else {
        return kSentinelValue;
    }
}

inline int Extract_b(uint32_t *item, int num_words, int64_t spacing) {
    return (item[(num_words - 1) * spacing] >> kBitsPerMulti_t) & ((1 << kBWTCharNumBits) - 1);
}

inline int ExtractCounting(uint32_t *item, int num_words, int64_t spacing) {
    return item[(num_words - 1) * spacing] & kMaxMulti_t;
}

// cx1 core functions
int64_t encode_lv1_diff_base(int64_t read_id, seq2sdbg_global_t &g) {
    assert(read_id < (int64_t)g.package.size());
    return EncodeEdgeOffset(read_id, 0, 0, g.package);
}

/**
 * @brief build lkt for faster binary search for mercy
 */
void InitLookupTable(int64_t *lookup_table, SequencePackage &p) {
    memset(lookup_table, 0xFF, sizeof(int64_t) * kLookUpSize * 2);
    if (p.size() == 0) {
        return;
    }

    Kmer<1, uint32_t> kmer;
    kmer.init(&p.packed_seq[0], 0, 16);

    uint32_t cur_prefix = kmer.data_[0] >> kLookUpShift;
    lookup_table[cur_prefix * 2] = 0;

    for (int64_t i = 1, num_edges = p.size(); i < num_edges; ++i) {
        kmer.init(&p.packed_seq[p.get_start_index(i) / 16], p.get_start_index(i) % 16, 16);

        if ((kmer.data_[0] >> kLookUpShift) > cur_prefix) {
            lookup_table[cur_prefix * 2 + 1] = i - 1;
            cur_prefix = kmer.data_[0] >> kLookUpShift;
            lookup_table[cur_prefix * 2] = i;
        } else {
            assert(cur_prefix == (kmer.data_[0] >> kLookUpShift));
        }
    }
    lookup_table[cur_prefix * 2 + 1] = p.size() - 1;
}

/**
 * @brief search mercy kmer
 */
inline int64_t BinarySearchKmer(GenericKmer &kmer, int64_t *lookup_table, SequencePackage &p, int kmer_size) {
    // --- first look up ---
    int64_t l = lookup_table[(kmer.data_[0] >> kLookUpShift) * 2];
    if (l == -1) {
        return -1;
    }
    int64_t r = lookup_table[(kmer.data_[0] >> kLookUpShift) * 2 + 1];
    GenericKmer mid_kmer;

    while (l <= r) {
        int64_t mid = (l + r) / 2;
        mid_kmer.init(&p.packed_seq[p.get_start_index(mid) / 16], p.get_start_index(mid) % 16, kmer_size);
        int cmp = kmer.cmp(mid_kmer, kmer_size);

        if (cmp > 0) {
            l = mid + 1;
        } else if (cmp < 0) {
            r = mid - 1;
        } else {
            return mid;
        }
    }

    return -1;
}

static void* MercyInputThread(void* seq_manager) {
    SequenceManager *sm = (SequenceManager*)seq_manager;
    int64_t kMaxReads = 1 << 22;
    int64_t kMaxBases = 1 << 28;
    bool append = false;
    bool reverse = false;
    sm->ReadShortReads(kMaxReads, kMaxBases, append, reverse);
    // printf("Processing %d reads\n", sm->package_->size());

    // if (sm->package_->size() > 0) {
    //     for (int i = 0; i < sm->package_->length(0); ++i)
    //         putchar("ACGT"[sm->package_->get_base(0, i)]);
    //     puts("");

    //     for (int i = 0; i < sm->package_->length(sm->package_->size() - 1); ++i)
    //         putchar("ACGT"[sm->package_->get_base(sm->package_->size() - 1, i)]);
    //     puts("");
    // }

    return NULL;
}

inline void GenMercyEdges(seq2sdbg_global_t &globals) {
    int64_t *edge_lookup = (int64_t *) MallocAndCheck(kLookUpSize * 2 * sizeof(int64_t), __FILE__, __LINE__);
    InitLookupTable(edge_lookup, globals.package);

    std::vector<GenericKmer > mercy_edges;

    SequencePackage read_package[2];
    SequenceManager seq_manager;
    seq_manager.set_file_type(SequenceManager::kBinaryReads);
    seq_manager.set_file(globals.input_prefix + ".cand");
    int thread_index = 0;
    seq_manager.set_package(&read_package[0]);

    pthread_t input_thread;
    pthread_create(&input_thread, NULL, MercyInputThread, &seq_manager);
    int num_threads = globals.num_cpu_threads - 1;
    omp_set_num_threads(num_threads);
    omp_lock_t mercy_lock;
    omp_init_lock(&mercy_lock);

    int64_t num_mercy_edges = 0;
    int64_t num_mercy_reads = 0;

    std::vector<bool> has_in, has_out;
    GenericKmer kmer, rev_kmer;

    while (true) {
        pthread_join(input_thread, NULL);
        SequencePackage &rp = read_package[thread_index];
        if (rp.size() == 0) {
            break;
        }

        thread_index ^= 1;
        seq_manager.set_package(&read_package[thread_index]);
        pthread_create(&input_thread, NULL, MercyInputThread, &seq_manager);

        num_mercy_reads += rp.size();
        mercy_edges.clear();

        #pragma omp parallel for reduction(+:num_mercy_edges) private(has_in, has_out, kmer, rev_kmer)
        for (unsigned read_id = 0; read_id < rp.size(); ++read_id) {

            int read_len = rp.length(read_id);
            if (read_len < globals.kmer_k + 2) {
                continue;
            }

            has_in.resize(read_len);
            has_out.resize(read_len);
            std::fill(has_in.begin(), has_in.end(), false);
            std::fill(has_out.begin(), has_out.end(), false);

            kmer.init(&rp.packed_seq[rp.get_start_index(read_id) / 16], rp.get_start_index(read_id) % 16, globals.kmer_k);
            rev_kmer = kmer;
            rev_kmer.ReverseComplement(globals.kmer_k);

            // mark those positions with in/out
            for (int i = 0; i + globals.kmer_k <= read_len; ++i) {
                if (!has_in[i]) {
                    // search rc
                    if (BinarySearchKmer(rev_kmer, edge_lookup, globals.package, globals.kmer_k) != -1) {
                        has_in[i] = true;
                    } else {
                        // left append ACGT to kmer, if the (k+1)-mer exist, the kmer has in
                        rev_kmer.set_base(globals.kmer_k, 3); // rev kmer is used to compare to kmer, if it's smaller, kmer would not exist in the table
                        kmer.ShiftPreappend(0, globals.kmer_k + 1);
                        for (int c = 0; c < 4; ++c) {
                            kmer.set_base(0, c);
                            if (kmer.cmp(rev_kmer, globals.kmer_k + 1) > 0) {
                                break;
                            }

                            if (BinarySearchKmer(kmer, edge_lookup, globals.package, globals.kmer_k + 1) != -1) {
                                has_in[i] = true;
                                break;
                            }
                        }
                        rev_kmer.set_base(globals.kmer_k, 0);
                        kmer.ShiftAppend(0, globals.kmer_k + 1); // clean the k+1-th char
                    }
                }

                // check whether has out
                int64_t edge_id = BinarySearchKmer(kmer, edge_lookup, globals.package, globals.kmer_k);
                if (edge_id != -1) {
                    has_out[i] = true;
                    // BWT see whether the next has in too
                    if (i + globals.kmer_k < read_len &&
                            globals.package.get_base(edge_id, globals.kmer_k) == rp.get_base(read_id, i + globals.kmer_k)) {
                        has_in[i + 1] = true;
                    }
                } else {
                    // search the rc
                    kmer.set_base(globals.kmer_k, 3);
                    int next_char = i + globals.kmer_k < read_len ? 3 - rp.get_base(read_id, i + globals.kmer_k) : 0;
                    rev_kmer.ShiftPreappend(next_char, globals.kmer_k + 1);

                    if (rev_kmer.cmp(kmer, globals.kmer_k + 1) <= 0 && BinarySearchKmer(rev_kmer, edge_lookup, globals.package, globals.kmer_k + 1) != -1) {
                        has_out[i] = true;
                        has_in[i + 1] = true;
                    } else {
                        for (int c = 0; c < 4; ++c) {
                            if (c == next_char) {
                                continue;
                            }
                            rev_kmer.set_base(0, c);
                            if (rev_kmer.cmp(kmer, globals.kmer_k + 1) > 0) {
                                break;
                            }

                            if (BinarySearchKmer(rev_kmer, edge_lookup, globals.package, globals.kmer_k + 1) != -1) {
                                has_out[i] = true;
                                break;
                            }
                        }
                    }

                    kmer.set_base(globals.kmer_k, 0);
                    rev_kmer.ShiftAppend(0, globals.kmer_k + 1);
                }

                // shift kmer and rev_kmer
                if (i + globals.kmer_k < read_len) {
                    int next_char = rp.get_base(read_id, i + globals.kmer_k);
                    kmer.ShiftAppend(next_char, globals.kmer_k);
                    rev_kmer.ShiftPreappend(3 - next_char, globals.kmer_k);
                }
            }

            // adding mercy edges
            int last_no_out = -1;

            for (int i = 0; i + globals.kmer_k <= read_len; ++i) {
                switch (has_in[i] | (int(has_out[i]) << 1)) {
                case 1: { // has incoming only
                    last_no_out = i;
                    break;
                }
                case 2: { // has outgoing only
                    if (last_no_out >= 0) {
                        for (int j = last_no_out; j < i; ++j) {
                            omp_set_lock(&mercy_lock);
                            mercy_edges.push_back(GenericKmer(&rp.packed_seq[rp.get_start_index(read_id) / 16], rp.get_start_index(read_id) % 16 + j, globals.kmer_k + 1));
                            omp_unset_lock(&mercy_lock);
                        }
                        num_mercy_edges += i - last_no_out;
                    }
                    last_no_out = -1;
                    break;
                }
                case 3: { // has in and out
                    last_no_out = -1;
                    break;
                }
                default: {
                    // do nothing
                    break;
                }
                }
            }
        }

        for (unsigned i = 0; i < mercy_edges.size(); ++i) {
            globals.package.AppendFixedLenSeq(mercy_edges[i].data_, globals.kmer_k + 1);
        }
    }

    globals.multiplicity.insert(globals.multiplicity.end(), num_mercy_edges, 1);

    if (cx1_t::kCX1Verbose >= 2) {
        xlog("Number of reads: %ld, Number of mercy edges: %ld\n", num_mercy_reads, num_mercy_edges);
    }
}

void read_seq_and_prepare(seq2sdbg_global_t &globals) {
    // --- init reader ---
    globals.package.set_fixed_len(globals.kmer_k + 1);
    SequenceManager seq_manager(&globals.package);
    seq_manager.set_multiplicity_vector(&globals.multiplicity);

    // reserve space
    {
        long long bases_to_reserve = 0;
        long long num_contigs_to_reserve = 0;
        long long num_multiplicities_to_reserve = 0;

        if (globals.input_prefix != "") {
            long long k_size, num_edges;
            FILE *edge_info = OpenFileAndCheck((globals.input_prefix + ".edges.info").c_str(), "r");
            assert(fscanf(edge_info, "%lld%lld", &k_size, &num_edges) == 2);
            if (globals.need_mercy) { num_edges *= 1.25; } // it is rare that # mercy > 25%
            bases_to_reserve += num_edges * (k_size + 1);
            num_multiplicities_to_reserve += num_edges;
            fclose(edge_info);
        }

        if (globals.contig != "") {
            long long num_contigs, num_bases;
            FILE *contig_info = OpenFileAndCheck((globals.contig + ".info").c_str(), "r");
            assert(fscanf(contig_info, "%lld%lld", &num_contigs, &num_bases) == 2);
            bases_to_reserve += num_bases;
            num_contigs_to_reserve += num_contigs;
            num_multiplicities_to_reserve += num_contigs;
            fclose(contig_info);
        }

        if (globals.addi_contig != "") {
            long long num_contigs, num_bases;
            FILE *contig_info = OpenFileAndCheck((globals.addi_contig + ".info").c_str(), "r");
            assert(fscanf(contig_info, "%lld%lld", &num_contigs, &num_bases) == 2);
            bases_to_reserve += num_bases;
            num_contigs_to_reserve += num_contigs;
            num_multiplicities_to_reserve += num_contigs;
            fclose(contig_info);
        }

        if (globals.local_contig != "") {
            long long num_contigs, num_bases;
            FILE *contig_info = OpenFileAndCheck((globals.local_contig + ".info").c_str(), "r");
            assert(fscanf(contig_info, "%lld%lld", &num_contigs, &num_bases) == 2);
            bases_to_reserve += num_bases;
            num_contigs_to_reserve += num_contigs;
            num_multiplicities_to_reserve += num_contigs;
            fclose(contig_info);
        }

        globals.package.reserve_num_seq(num_contigs_to_reserve);
        globals.package.reserve_bases(bases_to_reserve);
        globals.multiplicity.reserve(num_multiplicities_to_reserve);

    }

    if (globals.input_prefix != "") {
        seq_manager.set_file_type(globals.need_mercy ? SequenceManager::kSortedEdges : SequenceManager::kMegahitEdges);
        seq_manager.set_edge_files(globals.input_prefix + ".edges", globals.num_edge_files);
        seq_manager.ReadEdgesWithFixedLen(1LL << 60, true);
        seq_manager.clear();
    }

    if (globals.need_mercy) {
        xtimer_t timer;
        if (cx1_t::kCX1Verbose >= 3) {
            timer.reset();
            timer.start();
            xlog("Adding mercy edges...\n");
        }

        GenMercyEdges(globals);

        if (cx1_t::kCX1Verbose >= 3) {
            timer.stop();
            xlog("Done. Time elapsed: %.4lf\n", timer.elapsed());
        }
    }

    if (globals.contig != "") {
        seq_manager.set_file_type(SequenceManager::kMegahitContigs);
        seq_manager.set_file(globals.contig);
        seq_manager.set_kmer_size(globals.kmer_from, globals.kmer_k);
        seq_manager.set_min_len(globals.kmer_k + 1);

        bool contig_reverse = true;
        bool append_to_package = true;
        int discard_flag = 0;
        bool extend_loop = true;
        bool calc_depth = true;

        seq_manager.ReadMegahitContigs(1LL << 60, 1LL << 60, append_to_package, contig_reverse, discard_flag, extend_loop, calc_depth);
        seq_manager.clear();
    }

    if (globals.addi_contig != "") {
        seq_manager.set_file_type(SequenceManager::kMegahitContigs);
        seq_manager.set_file(globals.addi_contig);
        seq_manager.set_kmer_size(globals.kmer_from, globals.kmer_k);
        seq_manager.set_min_len(globals.kmer_k + 1);

        bool contig_reverse = true;
        bool append_to_package = true;
        int discard_flag = 0;
        bool extend_loop = true;
        bool calc_depth = false;

        seq_manager.ReadMegahitContigs(1LL << 60, 1LL << 60, append_to_package, contig_reverse, discard_flag, extend_loop, calc_depth);
        seq_manager.clear();
    }

    if (globals.local_contig != "") {
        seq_manager.set_file_type(SequenceManager::kMegahitContigs);
        seq_manager.set_file(globals.local_contig);
        seq_manager.set_kmer_size(globals.kmer_from, globals.kmer_k);
        seq_manager.set_min_len(globals.kmer_k + 1);

        bool contig_reverse = true;
        bool append_to_package = true;
        int discard_flag = 0;
        bool extend_loop = true;
        bool calc_depth = false;

        seq_manager.ReadMegahitContigs(1LL << 60, 1LL << 60, append_to_package, contig_reverse, discard_flag, extend_loop, calc_depth);
        seq_manager.clear();
    }

    globals.package.BuildLookup();
    globals.num_seq = globals.package.size();

    globals.mem_packed_seq = globals.package.size_in_byte() + globals.multiplicity.size() * sizeof(multi_t);
    int64_t mem_low_bound = globals.mem_packed_seq
                          + kNumBuckets * sizeof(int64_t) * (globals.num_cpu_threads * 3 + 1);
    mem_low_bound *= 1.05;

    if (mem_low_bound > globals.host_mem) {
        xerr_and_exit("%lld bytes is not enough for CX1 sorting, please set -m parameter to at least %lld\n", globals.host_mem, mem_low_bound);
    }

    // --- set cx1 param ---
    globals.cx1.num_cpu_threads_ = globals.num_cpu_threads;
    globals.cx1.num_output_threads_ = globals.num_output_threads;
    globals.cx1.num_items_ = globals.num_seq;
}

void* lv0_calc_bucket_size(void* _data) {
    readpartition_data_t &rp = *((readpartition_data_t*) _data);
    seq2sdbg_global_t &globals = *(rp.globals);
    int64_t *bucket_sizes = rp.rp_bucket_sizes;
    memset(bucket_sizes, 0, kNumBuckets * sizeof(int64_t));

    for (int64_t seq_id = rp.rp_start_id; seq_id < rp.rp_end_id; ++seq_id) {
        int seq_len = globals.package.length(seq_id);
        if (seq_len < globals.kmer_k + 1) {
            continue;
        }

        uint32_t key = 0; // $$$$$$$$
        // build initial partial key
        for (int i = 0; i < kBucketPrefixLength - 1; ++i) {
            key = key * kBucketBase + globals.package.get_base(seq_id, i) + 1;
        }
        // sequence = xxxxxxxxx
        // edges = $xxxx, xxxxx, ..., xxxx$
        for (int i = kBucketPrefixLength - 1; i - (kBucketPrefixLength - 1) + globals.kmer_k - 1 <= seq_len; ++i) {
            key = (key * kBucketBase + globals.package.get_base(seq_id, i) + 1) % kNumBuckets;
            bucket_sizes[key]++;
        }

        // reverse complement
        key = 0;
        for (int i = 0; i < kBucketPrefixLength - 1; ++i) {
            key = key * kBucketBase + (3 - globals.package.get_base(seq_id, seq_len - 1 - i)) + 1; // complement
        }
        for (int i = kBucketPrefixLength - 1; i - (kBucketPrefixLength - 1) + globals.kmer_k - 1 <= seq_len; ++i) {
            key = key * kBucketBase + (3 - globals.package.get_base(seq_id, seq_len - 1 - i)) + 1;
            key %= kNumBuckets;
            bucket_sizes[key]++;
        }
    }
    return NULL;
}

void init_global_and_set_cx1(seq2sdbg_global_t &globals) {
    // --- calculate lv2 memory ---
    globals.max_bucket_size = *std::max_element(globals.cx1.bucket_sizes_, globals.cx1.bucket_sizes_ + kNumBuckets);
    globals.tot_bucket_size = 0;
    for (int i = 0; i < kNumBuckets; ++i) {
        globals.tot_bucket_size += globals.cx1.bucket_sizes_[i];
    }
#ifndef USE_GPU
    globals.cx1.max_lv2_items_ = std::max(globals.max_bucket_size, kMinLv2BatchSize);
#else
    int64_t lv2_mem = globals.gpu_mem - 1073741824; // should reserve ~1G for GPU sorting
    globals.cx1.max_lv2_items_ = std::min(lv2_mem / cx1_t::kGPUBytePerItem, std::max(globals.max_bucket_size, kMinLv2BatchSizeGPU));
    if (globals.max_bucket_size > globals.cx1.max_lv2_items_) {
        xerr_and_exit("Bucket too large for GPU: contains %lld items. Please try CPU version.\n", globals.max_bucket_size);
        // TODO: auto switch to CPU version
        exit(1);
    }
#endif
    globals.words_per_substring = DivCeiling(globals.kmer_k * kBitsPerEdgeChar + kBWTCharNumBits + 1 + kBitsPerMulti_t, kBitsPerEdgeWord);
    globals.words_per_dummy_node = DivCeiling(globals.kmer_k * kBitsPerEdgeChar, kBitsPerEdgeWord);
    // lv2 bytes: substring (double buffer), permutation, aux
    int64_t lv2_bytes_per_item = (globals.words_per_substring * sizeof(uint32_t) + sizeof(uint32_t)) * 2 + sizeof(unsigned char);
#ifndef USE_GPU
    lv2_bytes_per_item += sizeof(uint64_t) * 2; // simulate GPU
#endif

    if (cx1_t::kCX1Verbose >= 2) {
        xlog("%d words per substring, num sequences: %ld, words per dummy node ($v): %d\n", globals.words_per_substring, globals.num_seq, globals.words_per_dummy_node);
    }

    // --- memory stuff ---
    int64_t mem_remained = globals.host_mem
                           - globals.mem_packed_seq
                           - kNumBuckets * sizeof(int64_t) * (globals.num_cpu_threads * 3 + 1);

    int64_t min_lv1_items = globals.tot_bucket_size / (kMaxLv1ScanTime - 0.5);
    int64_t min_lv2_items = std::max(globals.max_bucket_size, kMinLv2BatchSize);

    if (globals.mem_flag == 1) {
        // auto set memory
        globals.cx1.max_lv1_items_ = std::max(globals.cx1.max_lv2_items_, int64_t(globals.tot_bucket_size / (kDefaultLv1ScanTime - 0.5)));
        int64_t mem_needed = globals.cx1.max_lv1_items_ * cx1_t::kLv1BytePerItem + globals.cx1.max_lv2_items_ * lv2_bytes_per_item;
        if (mem_needed > mem_remained) {
            globals.cx1.adjust_mem(mem_remained, lv2_bytes_per_item, min_lv1_items, min_lv2_items);
        }
    } else if (globals.mem_flag == 0) {
        // min memory
        globals.cx1.max_lv1_items_ = std::max(globals.cx1.max_lv2_items_, int64_t(globals.tot_bucket_size / (kMaxLv1ScanTime - 0.5)));
        int64_t mem_needed = globals.cx1.max_lv1_items_ * cx1_t::kLv1BytePerItem + globals.cx1.max_lv2_items_ * lv2_bytes_per_item;
        if (mem_needed > mem_remained) {
            globals.cx1.adjust_mem(mem_remained, lv2_bytes_per_item, min_lv1_items, min_lv2_items);
        } else {
            globals.cx1.adjust_mem(mem_needed, lv2_bytes_per_item, min_lv1_items, min_lv2_items);
        }
    } else {
        // use all
        globals.cx1.adjust_mem(mem_remained, lv2_bytes_per_item, min_lv1_items, min_lv2_items);
    }

    if (cx1_t::kCX1Verbose >= 2) {
        xlog("Memory for sequence: %lld\n", globals.mem_packed_seq);
        xlog("max # lv.1 items = %lld\n", globals.cx1.max_lv1_items_);
        xlog("max # lv.2 items = %lld\n", globals.cx1.max_lv2_items_);
    }

    // --- alloc memory ---
    globals.lv1_items = (int*) MallocAndCheck(globals.cx1.max_lv1_items_ * sizeof(int), __FILE__, __LINE__);
    globals.lv2_substrings = (uint32_t*) MallocAndCheck(globals.cx1.max_lv2_items_ * globals.words_per_substring * sizeof(uint32_t), __FILE__, __LINE__);
    globals.permutation = (uint32_t *) MallocAndCheck(globals.cx1.max_lv2_items_ * sizeof(uint32_t), __FILE__, __LINE__);
    globals.lv2_substrings_db = (uint32_t*) MallocAndCheck(globals.cx1.max_lv2_items_ * globals.words_per_substring * sizeof(uint32_t), __FILE__, __LINE__);
    globals.permutation_db = (uint32_t *) MallocAndCheck(globals.cx1.max_lv2_items_ * sizeof(uint32_t), __FILE__, __LINE__);
    globals.lv2_aux = (unsigned char*) MallocAndCheck(globals.cx1.max_lv2_items_ * sizeof(unsigned char), __FILE__, __LINE__);
#ifndef USE_GPU
    globals.cpu_sort_space = (uint64_t*) MallocAndCheck(sizeof(uint64_t) * globals.cx1.max_lv2_items_, __FILE__, __LINE__);
#else
    alloc_gpu_buffers(globals.gpu_key_buffer1, globals.gpu_key_buffer2, globals.gpu_value_buffer1, globals.gpu_value_buffer2, (size_t)globals.cx1.max_lv2_items_);
#endif

    // --- init lock ---
    pthread_mutex_init(&globals.lv1_items_scanning_lock, NULL);

    // --- init stat ---
    globals.cur_prefix = -1;
    globals.cur_suffix_first_char = -1;
    globals.num_ones_in_last = 0;
    globals.total_number_edges = 0;
    globals.num_dollar_nodes = 0;
    memset(globals.num_chars_in_w, 0, sizeof(globals.num_chars_in_w));

    // --- init output ---
    globals.sdbg_writer.init((std::string(globals.output_prefix)+".w").c_str(),
                             (std::string(globals.output_prefix)+".last").c_str(),
                             (std::string(globals.output_prefix)+".isd").c_str());
    globals.dummy_nodes_writer.init((std::string(globals.output_prefix)+".dn").c_str());
    globals.output_f_file = OpenFileAndCheck((std::string(globals.output_prefix)+".f").c_str(), "w");
    globals.output_multiplicity_file = OpenFileAndCheck((std::string(globals.output_prefix)+".mul").c_str(), "wb");
    globals.output_multiplicity_file2 = OpenFileAndCheck((std::string(globals.output_prefix)+".mul2").c_str(), "wb");
    // --- write header ---
    fprintf(globals.output_f_file, "-1\n");
    globals.dummy_nodes_writer.output(globals.words_per_dummy_node);
}

void* lv1_fill_offset(void* _data) {
    readpartition_data_t &rp = *((readpartition_data_t*) _data);
    seq2sdbg_global_t &globals = *(rp.globals);
    int64_t *prev_full_offsets = (int64_t *)MallocAndCheck(kNumBuckets * sizeof(int64_t), __FILE__, __LINE__); // temporary array for computing differentials
    assert(prev_full_offsets != NULL);
    for (int b = globals.cx1.lv1_start_bucket_; b < globals.cx1.lv1_end_bucket_; ++b)
        prev_full_offsets[b] = rp.rp_lv1_differential_base;
    // this loop is VERY similar to that in PreprocessScanToFillBucketSizesThread

    // ===== this is a macro to save some copy&paste ================
#define CHECK_AND_SAVE_OFFSET(key_, offset, strand)                                                             \
    do {                                                                                                        \
        if (((key_ - globals.cx1.lv1_start_bucket_) ^ (key_ - globals.cx1.lv1_end_bucket_)) & kSignBitMask) {   \
            int64_t full_offset = EncodeEdgeOffset(seq_id, offset, strand, globals.package);                    \
            int64_t differential = full_offset - prev_full_offsets[key_];                                       \
            if (differential > cx1_t::kDifferentialLimit) {                                                     \
                pthread_mutex_lock(&globals.lv1_items_scanning_lock);                                           \
                globals.lv1_items[rp.rp_bucket_offsets[key_]++] = -globals.cx1.lv1_items_special_.size() - 1;   \
                globals.cx1.lv1_items_special_.push_back(full_offset);                                          \
                pthread_mutex_unlock(&globals.lv1_items_scanning_lock);                                         \
            } else {                                                                                            \
                assert(differential >= 0);                                                                      \
                globals.lv1_items[rp.rp_bucket_offsets[key_]++] = (int) differential;                           \
            }                                                                                                   \
            prev_full_offsets[key_] = full_offset;                                                              \
        }                                                                                                       \
    } while (0)
    // ^^^^^ why is the macro surrounded by a do-while? please ask Google
    // =========== end macro ==========================

    for (int64_t seq_id = rp.rp_start_id; seq_id < rp.rp_end_id; ++seq_id) {
        int seq_len = globals.package.length(seq_id);
        if (seq_len < globals.kmer_k + 1) {
            continue;
        }

        int key = 0; // $$$$$$$$
        int rev_key = 0;
        // build initial partial key
        for (int i = 0; i < kBucketPrefixLength - 1; ++i) {
            key = key * kBucketBase + globals.package.get_base(seq_id, i) + 1;
            rev_key = rev_key * kBucketBase + (3 - globals.package.get_base(seq_id, seq_len - 1 - i)) + 1; // complement
        }
        // sequence = xxxxxxxxx
        // edges = $xxxx, xxxxx, ..., xxxx$
        for (int i = kBucketPrefixLength - 1; i - (kBucketPrefixLength - 1) + globals.kmer_k - 1 <= seq_len; ++i) {
            key = (key * kBucketBase + globals.package.get_base(seq_id, i) + 1) % kNumBuckets;
            rev_key = rev_key * kBucketBase + (3 - globals.package.get_base(seq_id, seq_len - 1 - i)) + 1;
            rev_key %= kNumBuckets;
            CHECK_AND_SAVE_OFFSET(key, i - kBucketPrefixLength + 1, 0);
            CHECK_AND_SAVE_OFFSET(rev_key, i - kBucketPrefixLength + 1, 1);
        }
    }

#undef CHECK_AND_SAVE_OFFSET

    free(prev_full_offsets);
    return NULL;
}

// inline int BucketToPrefix(int x) {
//     int y = 0;
//     for (int i=0; i < kBucketPrefixLength; ++i) {
//         int z = x % kBucketBase;
//         if (z > 0) { --z; }
//         y |= (z << (i * kBitsPerEdgeChar));
//         x /= kBucketBase;
//     }
//     return y;
// }


void* lv2_extract_substr(void* _data) {
    bucketpartition_data_t &bp = *((bucketpartition_data_t*) _data);
    seq2sdbg_global_t &globals = *(bp.globals);
    int *lv1_p = globals.lv1_items + globals.cx1.rp_[0].rp_bucket_offsets[bp.bp_start_bucket];
    uint32_t *substrings_p = globals.lv2_substrings +
                             (globals.cx1.rp_[0].rp_bucket_offsets[bp.bp_start_bucket] - globals.cx1.rp_[0].rp_bucket_offsets[globals.cx1.lv2_start_bucket_]);

    for (int bucket = bp.bp_start_bucket; bucket < bp.bp_end_bucket; ++bucket) {
        for (int t = 0; t < globals.num_cpu_threads; ++t) {
            int64_t full_offset = globals.cx1.rp_[t].rp_lv1_differential_base;
            int num = globals.cx1.rp_[t].rp_bucket_sizes[bucket];
            for (int i = 0; i < num; ++i) {
                if (*lv1_p >= 0) {
                    full_offset += *(lv1_p++);
                } else {
                    full_offset = globals.cx1.lv1_items_special_[-1 - *(lv1_p++)];
                }
                int64_t seq_id = globals.package.get_id(full_offset >> 1);
                int offset = (full_offset >> 1) - globals.package.get_start_index(seq_id);
                int strand = full_offset & 1;

                int seq_len = globals.package.length(seq_id);
                int num_chars_to_copy = globals.kmer_k - (offset + globals.kmer_k > seq_len);
                int counting = 0;
                if (offset > 0 && offset + globals.kmer_k <= seq_len) {
                    counting = globals.multiplicity[seq_id];
                }

                int64_t which_word = globals.package.get_start_index(seq_id) / 16;
                int start_offset = globals.package.get_start_index(seq_id) % 16;
                int words_this_seq = DivCeiling(start_offset + seq_len, 16);
                uint32_t *edge_p = &globals.package.packed_seq[which_word];

                if (strand == 0) {
                    // copy counting and W char
                    int prev_char;
                    if (offset == 0) {
                        assert(num_chars_to_copy == globals.kmer_k);
                        prev_char = kSentinelValue;
                    } else {
                        prev_char = globals.package.get_base(seq_id, offset - 1);
                    }

                    CopySubstring(substrings_p, edge_p, offset + start_offset, num_chars_to_copy,
                                  globals.cx1.lv2_num_items_, words_this_seq, globals.words_per_substring);

                    uint32_t *last_word = substrings_p + int64_t(globals.words_per_substring - 1) * globals.cx1.lv2_num_items_;
                    *last_word |= int(num_chars_to_copy == globals.kmer_k) << (kBWTCharNumBits + kBitsPerMulti_t);
                    *last_word |= prev_char << kBitsPerMulti_t;
                    *last_word |= std::max(0, kMaxMulti_t - counting); // then larger counting come first after sorting
                } else {
                    int prev_char;
                    if (offset == 0) {
                        assert(num_chars_to_copy == globals.kmer_k);
                        prev_char = kSentinelValue;
                    } else {
                        prev_char = 3 - globals.package.get_base(seq_id, seq_len - 1 - offset + 1);
                    }

                    offset = seq_len - 1 - offset - (globals.kmer_k - 1); // switch to normal strand
                    if (offset < 0) {
                        assert(num_chars_to_copy == globals.kmer_k - 1);
                        offset = 0;
                    }
                    CopySubstringRC(substrings_p, edge_p, offset + start_offset, num_chars_to_copy,
                                    globals.cx1.lv2_num_items_, words_this_seq, globals.words_per_substring);

                    uint32_t *last_word = substrings_p + int64_t(globals.words_per_substring - 1) * globals.cx1.lv2_num_items_;
                    *last_word |= int(num_chars_to_copy == globals.kmer_k) << (kBWTCharNumBits + kBitsPerMulti_t);
                    *last_word |= prev_char << kBitsPerMulti_t;
                    *last_word |= std::max(0, kMaxMulti_t - counting);
                }

                // if ((*substrings_p >> (32 - kBucketPrefixLength * 2)) != BucketToPrefix(bucket)) {
                //     xwarning("WRONG substring wrong:%d right:%d read_id:%lld offset:%d strand: %d num_chars_to_copy:%d\n", *substrings_p >> (32 - kBucketPrefixLength * 2),  BucketToPrefix(bucket), seq_id, offset, strand, num_chars_to_copy);
                //     GenericKmer kmer;
                //     kmer.init(edge_p, offset + start_offset, kBucketPrefixLength);
                //     xwarning("%d\n", kmer.data_[0] >> (32 - kBucketPrefixLength * 2));
                // }
                substrings_p++;
            }
        }
    }
    return NULL;
}

void lv2_sort(seq2sdbg_global_t &globals) {
    xtimer_t local_timer;
#ifndef USE_GPU
    if (cx1_t::kCX1Verbose >= 4) {
        local_timer.reset();
        local_timer.start();
    }
    omp_set_num_threads(globals.num_cpu_threads - globals.num_output_threads);
    lv2_cpu_sort(globals.lv2_substrings, globals.permutation, globals.cpu_sort_space, globals.words_per_substring, globals.cx1.lv2_num_items_);
    omp_set_num_threads(globals.num_cpu_threads);
    local_timer.stop();

    if (cx1_t::kCX1Verbose >= 4) {
        xlog("Sorting substrings with CPU...done. Time elapsed: %.4lf\n", local_timer.elapsed());
    }
#else
    if (cx1_t::kCX1Verbose >= 4) {
        local_timer.reset();
        local_timer.start();
    }
    lv2_gpu_sort(globals.lv2_substrings, globals.permutation, globals.words_per_substring, globals.cx1.lv2_num_items_,
                 globals.gpu_key_buffer1, globals.gpu_key_buffer2, globals.gpu_value_buffer1, globals.gpu_value_buffer2);

    if (cx1_t::kCX1Verbose >= 4) {
        local_timer.stop();
        xlog("Sorting substrings with GPU...done. Time elapsed: %.4lf\n", local_timer.elapsed());
    }
#endif
}

void lv2_pre_output_partition(seq2sdbg_global_t &globals) {
    // swap double buffers
    globals.lv2_num_items_db = globals.cx1.lv2_num_items_;
    std::swap(globals.lv2_substrings_db, globals.lv2_substrings);
    std::swap(globals.permutation_db, globals.permutation);

    // distribute threads
    int64_t last_end_index = 0;
    int64_t items_per_thread = globals.lv2_num_items_db / globals.num_output_threads;

    for (int t = 0; t < globals.num_output_threads - 1; ++t) {
        int64_t this_start_index = last_end_index;
        int64_t this_end_index = this_start_index + items_per_thread;
        if (this_end_index > globals.lv2_num_items_db) {
            this_end_index = globals.lv2_num_items_db;
        }
        if (this_end_index > 0) {
            while (this_end_index < globals.lv2_num_items_db) {
                uint32_t *prev_item = globals.lv2_substrings_db + globals.permutation_db[this_end_index - 1];
                uint32_t *item = globals.lv2_substrings_db + globals.permutation_db[this_end_index];
                if (IsDiffKMinusOneMer(item, prev_item, globals.lv2_num_items_db, globals.kmer_k)) {
                    break;
                }
                ++this_end_index;
            }
        }
        globals.cx1.op_[t].op_start_index = this_start_index;
        globals.cx1.op_[t].op_end_index = this_end_index;
        last_end_index = this_end_index;
    }

    // last partition
    globals.cx1.op_[globals.num_output_threads - 1].op_start_index = last_end_index;
    globals.cx1.op_[globals.num_output_threads - 1].op_end_index = globals.lv2_num_items_db;

    memset(globals.lv2_aux, 0, sizeof(globals.lv2_aux[0]) * globals.lv2_num_items_db);
    pthread_barrier_init(&globals.output_barrier, NULL, globals.num_output_threads);
}

void* lv2_output(void* _op) {
    outputpartition_data_t *op = (outputpartition_data_t*) _op;
    seq2sdbg_global_t &globals = *(op->globals);
    int64_t op_start_index = op->op_start_index;
    int64_t op_end_index = op->op_end_index;
    int start_idx, end_idx;
    int has_solid_a = 0; // has solid (k+1)-mer aSb
    int has_solid_b = 0; // has solid aSb
    int last_a[4], outputed_b;

    for (start_idx = op_start_index; start_idx < op_end_index; start_idx = end_idx) {
        end_idx = start_idx + 1;
        uint32_t *item = globals.lv2_substrings_db + globals.permutation_db[start_idx];
        while (end_idx < op_end_index &&
                !IsDiffKMinusOneMer(
                    item,
                    globals.lv2_substrings_db + globals.permutation_db[end_idx],
                    globals.lv2_num_items_db,
                    globals.kmer_k)) {
            ++end_idx;
        }

        // clean marking
        has_solid_a = has_solid_b = 0;
        outputed_b = 0;
        for (int i = start_idx; i < end_idx; ++i) {
            uint32_t *cur_item = globals.lv2_substrings_db + globals.permutation_db[i];
            int a = Extract_a(cur_item, globals.words_per_substring, globals.lv2_num_items_db, globals.kmer_k);
            int b = Extract_b(cur_item, globals.words_per_substring, globals.lv2_num_items_db);

            if (a != kSentinelValue && b != kSentinelValue) {
                has_solid_a |= 1 << a;
                has_solid_b |= 1 << b;
            }
            if (a != kSentinelValue &&
                    (b != kSentinelValue || !(has_solid_a & (1 << a)))) {
                last_a[a] = i;
            }
        }

        for (int i = start_idx, j; i < end_idx; i = j) {
            uint32_t *cur_item = globals.lv2_substrings_db + globals.permutation_db[i];
            int a = Extract_a(cur_item, globals.words_per_substring, globals.lv2_num_items_db, globals.kmer_k);
            int b = Extract_b(cur_item, globals.words_per_substring, globals.lv2_num_items_db);

            j = i + 1;
            while (j < end_idx) {
                uint32_t *next_item = globals.lv2_substrings_db + globals.permutation_db[j];
                if (Extract_a(next_item, globals.words_per_substring, globals.lv2_num_items_db, globals.kmer_k) != a ||
                        Extract_b(next_item, globals.words_per_substring, globals.lv2_num_items_db) != b) {
                    break;
                } else {
                    ++j;
                }
            }

            int w, last, is_dollar = 0;

            if (a == kSentinelValue) {
                assert(b != kSentinelValue);
                if (has_solid_b & (1 << b)) {
                    continue;
                }
                is_dollar = 1;
            }

            if (b == kSentinelValue) {
                assert(a != kSentinelValue);
                if (has_solid_a & (1 << a)) {
                    continue;
                }
            }

            w = (b == kSentinelValue) ? 0 : ((outputed_b & (1 << b)) ? b + 5 : b + 1);
            outputed_b |= 1 << b;
            last = (a == kSentinelValue) ? 0 : ((last_a[a] == j - 1) ? 1 : 0);

            assert(!(globals.lv2_aux[i] & (1 << 7)));
            globals.lv2_aux[i] = w | (last << 4) | (is_dollar << 5) | (1 << 7);
        }
    }

    pthread_barrier_wait(&globals.output_barrier);

    if (op_start_index == 0) {
        xtimer_t local_timer;
        local_timer.reset();
        local_timer.start();
        for (int i = 0; i < globals.lv2_num_items_db; ++i) {
            if (globals.lv2_aux[i] & (1 << 7)) {
                uint32_t *item = globals.lv2_substrings_db + globals.permutation_db[i];
                while (ExtractFirstChar(item) > globals.cur_suffix_first_char) {
                    ++globals.cur_suffix_first_char;
                    fprintf(globals.output_f_file, "%lld\n", (long long)globals.total_number_edges);
                }

                multi_t counting_db = kMaxMulti_t - std::min(kMaxMulti_t,
                                      ExtractCounting(item, globals.words_per_substring, globals.lv2_num_items_db));
                // output
                globals.sdbg_writer.outputW(globals.lv2_aux[i] & 0xF);
                globals.sdbg_writer.outputLast((globals.lv2_aux[i] >> 4) & 1);
                globals.sdbg_writer.outputIsDollar((globals.lv2_aux[i] >> 5) & 1);
                if (counting_db <= kMaxMulti2_t) {
                    multi2_t c = counting_db;
                    fwrite(&c, sizeof(multi2_t), 1, globals.output_multiplicity_file);
                } else {
                    int64_t c = counting_db | (globals.total_number_edges << 16);
                    fwrite(&c, sizeof(int64_t), 1, globals.output_multiplicity_file2);
                    fwrite(&kMulti2Sp, sizeof(multi2_t), 1, globals.output_multiplicity_file);
                }

                globals.total_number_edges++;
                globals.num_chars_in_w[globals.lv2_aux[i] & 0xF]++;
                globals.num_ones_in_last += (globals.lv2_aux[i] >> 4) & 1;

                if ((globals.lv2_aux[i] >> 5) & 1) {
                    globals.num_dollar_nodes++;
                    if (globals.num_dollar_nodes >= kMaxDummyEdges) {
                        xerr_and_exit("Too many dummy nodes (>= %lld)! The graph contains too many tips!\n", (long long)kMaxDummyEdges);
                    }
                    for (int64_t i = 0; i < globals.words_per_dummy_node; ++i) {
                        globals.dummy_nodes_writer.output(item[i * globals.lv2_num_items_db]);
                    }
                }
                if ((globals.lv2_aux[i] & 0xF) == 0) {
                    globals.num_dummy_edges++;
                }
            }
        }
        local_timer.stop();

        if (cx1_t::kCX1Verbose >= 4) {
            xlog("SdBG calc linear part: %lf\n", local_timer.elapsed());
        }
    }
    return NULL;
}

void lv2_post_output(seq2sdbg_global_t &globals) {
    pthread_barrier_destroy(&globals.output_barrier);
}

void post_proc(seq2sdbg_global_t &globals) {
    if (cx1_t::kCX1Verbose >= 2) {
        xlog("Number of $ A C G T A- C- G- T-:\n");
    }

    xlog("");
    for (int i = 0; i < 9; ++i) {
        xlog_ext("%lld ", globals.num_chars_in_w[i]);
    }
    xlog_ext("\n");

    // --- write tails ---
    fprintf(globals.output_f_file, "%lld\n", (long long)globals.total_number_edges);
    fprintf(globals.output_f_file, "%d\n", globals.kmer_k);
    fprintf(globals.output_f_file, "%lld\n", (long long)globals.num_dollar_nodes);

    if (cx1_t::kCX1Verbose >= 2) {
        xlog("Total number of edges: %llu\n", globals.total_number_edges);
        xlog("Total number of ONEs: %llu\n", globals.num_ones_in_last);
        xlog("Total number of v$ edges: %llu\n", globals.num_dummy_edges);
        xlog("Total number of $v edges: %llu\n", globals.num_dollar_nodes);
    }

    // --- cleaning ---
    pthread_mutex_destroy(&globals.lv1_items_scanning_lock);
    free(globals.lv1_items);
    free(globals.lv2_substrings);
    free(globals.permutation);
    free(globals.lv2_substrings_db);
    free(globals.permutation_db);
    free(globals.lv2_aux);
    fclose(globals.output_f_file);
    fclose(globals.output_multiplicity_file);
    fclose(globals.output_multiplicity_file2);
    globals.dummy_nodes_writer.destroy();
#ifndef USE_GPU
    free(globals.cpu_sort_space);
#else
    free_gpu_buffers(globals.gpu_key_buffer1, globals.gpu_key_buffer2, globals.gpu_value_buffer1, globals.gpu_value_buffer2);
#endif
}

} // namespace