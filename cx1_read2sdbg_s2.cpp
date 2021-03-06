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

#include "cx1_read2sdbg.h"

#include <omp.h>
#include <string>
#include <vector>
#include <parallel/algorithm>

#include "utils.h"
#include "kmer.h"
#include "sdbg_builder_writers.h"
#include "mem_file_checker-inl.h"
#include "packed_reads.h"
#include "mac_pthread_barrier.h"
#include "read_lib_functions-inl.h"

#ifndef USE_GPU
#include "lv2_cpu_sort.h"
#else
#include "lv2_gpu_functions.h"
#endif

namespace cx1_read2sdbg {

namespace s2 {

typedef CX1<read2sdbg_global_t, kNumBuckets> cx1_t;
typedef CX1<read2sdbg_global_t, kNumBuckets>::readpartition_data_t readpartition_data_t;
typedef CX1<read2sdbg_global_t, kNumBuckets>::bucketpartition_data_t bucketpartition_data_t;
typedef CX1<read2sdbg_global_t, kNumBuckets>::outputpartition_data_t outputpartition_data_t;

// helper functions
inline int64_t EncodeOffset(int64_t read_id, int offset, int strand, int length_num_bits, int edge_type) {
    // edge_type: 0 left $; 1 solid; 2 right $
    return (read_id << (length_num_bits + 3)) | (offset << 3) | (edge_type << 1) | strand;
}

// helper: see whether two lv2 items have the same (k-1)-mer
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

// helper
inline int ExtractFirstChar(uint32_t *item) {
    return *item >> kTopCharShift;
}

// bS'a
inline int Extract_a(uint32_t *item, int num_words, int64_t spacing, int kmer_k) {
    int non_dollar = (item[(num_words - 1) * spacing] >> kBWTCharNumBits) & 1;
    if (non_dollar) {
        int which_word = (kmer_k - 1) / kCharsPerEdgeWord;
        int word_index = (kmer_k - 1) % kCharsPerEdgeWord;
        return (item[which_word * spacing] >> (kCharsPerEdgeWord - 1 - word_index) * kBitsPerEdgeChar) & kEdgeCharMask;
    } else {
        return kSentinelValue;
    }
}

inline int Extract_b(uint32_t *item, int num_words, int64_t spacing) {
    return item[(num_words - 1) * spacing] & ((1 << kBWTCharNumBits) - 1);
}


// cx1 core functions
int64_t s2_encode_lv1_diff_base(int64_t read_id, read2sdbg_global_t &globals) {
    return EncodeOffset(read_id, 0, 0, globals.offset_num_bits, 0);
}

void s2_read_mercy_prepare(read2sdbg_global_t &globals) {
    if (!globals.need_mercy) return;

    xtimer_t timer;
    if (cx1_t::kCX1Verbose >= 3) {
        timer.reset();
        timer.start();
        xlog("Adding mercy edges...\n");
    }

    std::vector<uint64_t> mercy_cand;
    uint64_t offset_mask = (1 << globals.offset_num_bits) - 1; // 0000....00011..11
    uint64_t num_mercy = 0;
    AtomicBitVector read_marker;
    read_marker.reset(globals.num_reads);

    for (int fid = 0; fid < globals.num_mercy_files; ++fid) {
        FILE *fp = OpenFileAndCheck(FormatString("%s.mercy_cand.%d", globals.output_prefix.c_str(), fid), "rb");
        mercy_cand.clear();

        int num_read = 0;
        uint64_t buf[4096];
        while ((num_read = fread(buf, sizeof(uint64_t), 4096, fp)) > 0) {
            mercy_cand.insert(mercy_cand.end(), buf, buf + num_read);
        }
        if (cx1_t::kCX1Verbose >= 4) {
            xlog("Mercy file: %s, %lu\n", FormatString("%s.mercy_cand.%d", globals.output_prefix.c_str(), fid), mercy_cand.size());
        }

        omp_set_num_threads(globals.num_cpu_threads);
        __gnu_parallel::sort(mercy_cand.begin(), mercy_cand.end());

        // multi threading
        uint64_t avg = DivCeiling(mercy_cand.size(), globals.num_cpu_threads);
        std::vector<uint64_t> start_idx(globals.num_cpu_threads);
        std::vector<uint64_t> end_idx(globals.num_cpu_threads);

        // manually distribute threads
        for (int tid = 0; tid < globals.num_cpu_threads; ++tid) {
            if (tid == 0) {
                start_idx[tid] = 0;
            } else {
                start_idx[tid] = end_idx[tid - 1];
            }

            uint64_t this_end = std::min(start_idx[tid] + avg, (uint64_t)mercy_cand.size());
            uint64_t read_id = mercy_cand[this_end] >> (globals.offset_num_bits + 2);
            while (this_end < mercy_cand.size() && (mercy_cand[this_end] >> (globals.offset_num_bits + 2)) == read_id) {
                ++this_end;
            }
            end_idx[tid] = this_end;
        }

        #pragma omp parallel for reduction(+:num_mercy)
        for (int tid = 0; tid < globals.num_cpu_threads; ++tid) {
            std::vector<bool> no_in(globals.max_read_length);
            std::vector<bool> no_out(globals.max_read_length);
            std::vector<bool> has_solid_kmer(globals.max_read_length);

            uint64_t i = start_idx[tid];
            // go read by read
            while (i != end_idx[tid]) {
                uint64_t read_id = mercy_cand[i] >> (globals.offset_num_bits + 2);
                assert(!read_marker.get(read_id));
                read_marker.set(read_id);
                int first_0_out = globals.max_read_length + 1;
                int last_0_in = -1;

                std::fill(no_in.begin(), no_in.end(), false);
                std::fill(no_out.begin(), no_out.end(), false);
                std::fill(has_solid_kmer.begin(), has_solid_kmer.end(), false);

                while (i != end_idx[tid] && (mercy_cand[i] >> (globals.offset_num_bits + 2)) == read_id) {
                    if ((mercy_cand[i] & 3) == 2) {
                        no_out[(mercy_cand[i] >> 2) & offset_mask] = true;
                        first_0_out = std::min(first_0_out, int((mercy_cand[i] >> 2) & offset_mask));
                    } else if ((mercy_cand[i] & 3) == 1) {
                        no_in[(mercy_cand[i] >> 2) & offset_mask] = true;
                        last_0_in = std::max(last_0_in, int((mercy_cand[i] >> 2) & offset_mask));
                    }
                    has_solid_kmer[int((mercy_cand[i] >> 2) & offset_mask)] = true;
                    ++i;
                }
                if (last_0_in < first_0_out) {
                    continue;
                }

                int read_length = globals.package.length(read_id);
                int last_no_out = -1;

                for (int i = 0; i + globals.kmer_k < read_length; ++i) {
                    if (globals.is_solid.get(read_id * globals.num_k1_per_read + i)) {
                        has_solid_kmer[i] = has_solid_kmer[i + 1] = true;
                    }
                }

                for (int i = 0; i + globals.kmer_k <= read_length; ++i) {
                    if (no_in[i] && last_no_out != -1) {
                        for (int j = last_no_out; j < i; ++j) {
                            globals.is_solid.set(read_id * globals.num_k1_per_read + j);
                        }
                        num_mercy += i - last_no_out;
                    }
                    if (has_solid_kmer[i]) {
                        last_no_out = -1;
                    }
                    if (no_out[i]) {
                        last_no_out = i;
                    }
                }

            }
        }

        fclose(fp);
    }

    if (cx1_t::kCX1Verbose >= 3) {
        timer.stop();
        xlog("Adding mercy Done. Time elapsed: %.4lf\n", timer.elapsed());
        xlog("Number mercy: %llu\n", (unsigned long long)num_mercy);
    }

    // set cx1 param
    globals.cx1.num_cpu_threads_ = globals.num_cpu_threads;
    globals.cx1.num_output_threads_ = globals.num_output_threads;
    globals.cx1.num_items_ = globals.num_reads;
}

void* s2_lv0_calc_bucket_size(void* _data) {
    readpartition_data_t &rp = *((readpartition_data_t*) _data);
    read2sdbg_global_t &globals = *(rp.globals);
    int64_t *bucket_sizes = rp.rp_bucket_sizes;
    memset(bucket_sizes, 0, kNumBuckets * sizeof(int64_t));
    GenericKmer edge, rev_edge; // (k+1)-mer and its rc

    for (int64_t read_id = rp.rp_start_id; read_id < rp.rp_end_id; ++read_id) {
        int read_length = globals.package.length(read_id);
        if (read_length < globals.kmer_k + 1) {
            continue;
        }

        int64_t which_word = globals.package.get_start_index(read_id) / 16;
        int64_t offset = globals.package.get_start_index(read_id) % 16;
        uint32_t *read_p = &globals.package.packed_seq[which_word];

        edge.init(read_p, offset, globals.kmer_k + 1);
        rev_edge = edge;
        rev_edge.ReverseComplement(globals.kmer_k + 1);

        int last_char_offset = globals.kmer_k;
        int64_t full_offset = globals.num_k1_per_read * read_id;
        while (true) {
            if (globals.is_solid.get(full_offset)) {
                bool is_palindrome = rev_edge.cmp(edge, globals.kmer_k + 1) == 0;
                bucket_sizes[(edge.data_[0] << 2) >> (kCharsPerEdgeWord - kBucketPrefixLength) * kBitsPerEdgeChar]++;
                if (!is_palindrome)
                    bucket_sizes[(rev_edge.data_[0] << 2) >> (kCharsPerEdgeWord - kBucketPrefixLength) * kBitsPerEdgeChar]++;

                if (last_char_offset == globals.kmer_k || !globals.is_solid.get(full_offset - 1)) {
                    bucket_sizes[edge.data_[0] >> (kCharsPerEdgeWord - kBucketPrefixLength) * kBitsPerEdgeChar]++;
                    if (!is_palindrome)
                        bucket_sizes[(rev_edge.data_[0] << 4) >> (kCharsPerEdgeWord - kBucketPrefixLength) * kBitsPerEdgeChar]++;
                }

                if (last_char_offset == read_length - 1 || !globals.is_solid.get(full_offset + 1)) {
                    bucket_sizes[(edge.data_[0] << 4) >> (kCharsPerEdgeWord - kBucketPrefixLength) * kBitsPerEdgeChar]++;
                    if (!is_palindrome)
                        bucket_sizes[rev_edge.data_[0] >> (kCharsPerEdgeWord - kBucketPrefixLength) * kBitsPerEdgeChar]++;
                }
            }

            ++full_offset;

            if (++last_char_offset >= read_length) {
                break;
            } else {
                int c = globals.package.get_base(read_id, last_char_offset);
                edge.ShiftAppend(c, globals.kmer_k + 1);
                rev_edge.ShiftPreappend(3 - c, globals.kmer_k + 1);
            }
        }
    }
    return NULL;
}

void s2_init_global_and_set_cx1(read2sdbg_global_t &globals) {
    // --- fill bucket size ---
    xtimer_t timer;
    timer.reset();
    timer.start();

    globals.max_bucket_size = *std::max_element(globals.cx1.bucket_sizes_, globals.cx1.bucket_sizes_ + kNumBuckets);
    globals.tot_bucket_size = 0;
    for (int i = 0; i < kNumBuckets; ++i) {
        globals.tot_bucket_size += globals.cx1.bucket_sizes_[i];
    }
    timer.stop();

    if (cx1_t::kCX1Verbose >= 3) {
        xlog("Done. Time elapsed: %.4lfs\n", timer.elapsed());
    }

    // --- calculate lv2 memory ---
#ifndef USE_GPU
    globals.cx1.max_lv2_items_ = std::max(globals.max_bucket_size, kMinLv2BatchSize);
#else
    int64_t lv2_mem = globals.gpu_mem - 1073741824; // should reserver ~1G for GPU sorting
    globals.cx1.max_lv2_items_ = std::min(lv2_mem / cx1_t::kGPUBytePerItem, std::max(globals.max_bucket_size, kMinLv2BatchSizeGPU));
    if (globals.max_bucket_size > globals.cx1.max_lv2_items_) {
        xerr_and_exit("Bucket too large for GPU: contains %lld items. Please try CPU version.\n", globals.max_bucket_size);
        // TODO: auto switch to CPU version
    }
#endif

    globals.words_per_substring = DivCeiling(globals.kmer_k * kBitsPerEdgeChar + kBWTCharNumBits + 1, kBitsPerEdgeWord);
    globals.words_per_dummy_node = DivCeiling(globals.kmer_k * kBitsPerEdgeChar, kBitsPerEdgeWord);
    // lv2 bytes: substring (double buffer), permutation, aux
    int64_t lv2_bytes_per_item = (globals.words_per_substring * sizeof(uint32_t) + sizeof(uint32_t)) * 2 + sizeof(int64_t);
#ifndef USE_GPU
    lv2_bytes_per_item += sizeof(uint64_t) * 2; // simulate GPU
#endif

    if (cx1_t::kCX1Verbose >= 2) {
        xlog("%d words per substring, words per dummy node ($v): %d\n", globals.words_per_substring, globals.words_per_dummy_node);
    }

    // --- memory stuff ---
    int64_t mem_remained = globals.host_mem
                           - globals.mem_packed_reads
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
        xlog("Memory for reads: %lld\n", globals.mem_packed_reads);
        xlog("max # lv.1 items = %lld\n", globals.cx1.max_lv1_items_);
        xlog("max # lv.2 items = %lld\n", globals.cx1.max_lv2_items_);
    }

    // --- alloc memory ---
    globals.lv1_items = (int*) MallocAndCheck(globals.cx1.max_lv1_items_ * sizeof(int), __FILE__, __LINE__);
    globals.lv2_substrings = (uint32_t*) MallocAndCheck(globals.cx1.max_lv2_items_ * globals.words_per_substring * sizeof(uint32_t), __FILE__, __LINE__);
    globals.permutation = (uint32_t *) MallocAndCheck(globals.cx1.max_lv2_items_ * sizeof(uint32_t), __FILE__, __LINE__);
    globals.lv2_substrings_db = (uint32_t*) MallocAndCheck(globals.cx1.max_lv2_items_ * globals.words_per_substring * sizeof(uint32_t), __FILE__, __LINE__);
    globals.permutation_db = (uint32_t *) MallocAndCheck(globals.cx1.max_lv2_items_ * sizeof(uint32_t), __FILE__, __LINE__);
#ifndef USE_GPU
    globals.cpu_sort_space = (uint64_t*) MallocAndCheck(sizeof(uint64_t) * globals.cx1.max_lv2_items_, __FILE__, __LINE__);
#else
    alloc_gpu_buffers(globals.gpu_key_buffer1, globals.gpu_key_buffer2, globals.gpu_value_buffer1, globals.gpu_value_buffer2, (size_t)globals.cx1.max_lv2_items_);
#endif
    globals.lv2_output_items.resize(globals.num_output_threads);

    pthread_mutex_init(&globals.lv1_items_scanning_lock, NULL); // init lock
    // --- init output ---
    globals.sdbg_writer.init((std::string(globals.output_prefix)+".w").c_str(),
                             (std::string(globals.output_prefix)+".last").c_str(),
                             (std::string(globals.output_prefix)+".isd").c_str());
    globals.dummy_nodes_writer.init((std::string(globals.output_prefix)+".dn").c_str());
    globals.output_f_file = OpenFileAndCheck((std::string(globals.output_prefix)+".f").c_str(), "w");
    globals.output_multiplicity_file = OpenFileAndCheck((std::string(globals.output_prefix)+".mul").c_str(), "wb");
    globals.output_multiplicity_file2 = OpenFileAndCheck((std::string(globals.output_prefix)+".mul2").c_str(), "wb");

    // --- init stat ---
    globals.cur_prefix = -1;
    globals.cur_suffix_first_char = -1;
    globals.num_ones_in_last = 0;
    globals.total_number_edges = 0;
    globals.num_dollar_nodes = 0;
    memset(globals.num_chars_in_w, 0, sizeof(globals.num_chars_in_w));

    // --- write header ---
    fprintf(globals.output_f_file, "-1\n");
    globals.dummy_nodes_writer.output(globals.words_per_dummy_node);
}

void* s2_lv1_fill_offset(void* _data) {
    readpartition_data_t &rp = *((readpartition_data_t*) _data);
    read2sdbg_global_t &globals = *(rp.globals);
    int64_t *prev_full_offsets = (int64_t *)MallocAndCheck(kNumBuckets * sizeof(int64_t), __FILE__, __LINE__); // temporary array for computing differentials
    assert(prev_full_offsets != NULL);
    for (int b = globals.cx1.lv1_start_bucket_; b < globals.cx1.lv1_end_bucket_; ++b)
        prev_full_offsets[b] = rp.rp_lv1_differential_base;
    // this loop is VERY similar to that in PreprocessScanToFillBucketSizesThread
    GenericKmer edge, rev_edge; // (k+1)-mer and its rc

    int key;
    for (int64_t read_id = rp.rp_start_id; read_id < rp.rp_end_id; ++read_id) {
        int read_length = globals.package.length(read_id);
        if (read_length < globals.kmer_k + 1) {
            continue;
        }

        int64_t which_word = globals.package.get_start_index(read_id) / 16;
        int64_t offset = globals.package.get_start_index(read_id) % 16;
        uint32_t *read_p = &globals.package.packed_seq[which_word];

        edge.init(read_p, offset, globals.kmer_k + 1);
        rev_edge = edge;
        rev_edge.ReverseComplement(globals.kmer_k + 1);

        // ===== this is a macro to save some copy&paste ================
#define CHECK_AND_SAVE_OFFSET(offset, strand, edge_type)                                                                    \
        do {                                                                                                                \
            assert(edge_type < 3 && edge_type >= 0 && strand >= 0 && strand <= 1 && offset + globals.kmer_k < read_length); \
            if (((key - globals.cx1.lv1_start_bucket_) ^ (key - globals.cx1.lv1_end_bucket_)) & kSignBitMask) {             \
                int64_t full_offset = EncodeOffset(read_id, offset, strand, globals.offset_num_bits, edge_type);            \
                int64_t differential = full_offset - prev_full_offsets[key];                                                \
                if (differential > cx1_t::kDifferentialLimit) {                                                             \
                    pthread_mutex_lock(&globals.lv1_items_scanning_lock);                                                   \
                    globals.lv1_items[rp.rp_bucket_offsets[key]++] = -globals.cx1.lv1_items_special_.size() - 1;            \
                    globals.cx1.lv1_items_special_.push_back(full_offset);                                                  \
                    pthread_mutex_unlock(&globals.lv1_items_scanning_lock);                                                 \
                } else {                                                                                                    \
                    assert ((int) differential >= 0);                                                                       \
                    globals.lv1_items[rp.rp_bucket_offsets[key]++] = (int) differential;                                    \
                }                                                                                                           \
                prev_full_offsets[key] = full_offset;                                                                       \
            }                                                                                                               \
        } while (0)
        // ^^^^^ why is the macro surrounded by a do-while? please ask Google
        // =========== end macro ==========================

        // shift the key char by char
        int last_char_offset = globals.kmer_k;
        int64_t full_offset = globals.num_k1_per_read * read_id;
        while (true) {
            if (globals.is_solid.get(full_offset)) {
                bool is_palindrome = rev_edge.cmp(edge, globals.kmer_k + 1) == 0;

                // left $
                if (last_char_offset == globals.kmer_k || !globals.is_solid.get(full_offset - 1)) {
                    key = edge.data_[0] >> (kCharsPerEdgeWord - kBucketPrefixLength) * kBitsPerEdgeChar;
                    CHECK_AND_SAVE_OFFSET(last_char_offset - globals.kmer_k, 0, 0);
                    if (!is_palindrome) {
                        key = (rev_edge.data_[0] << 4) >> (kCharsPerEdgeWord - kBucketPrefixLength) * kBitsPerEdgeChar;
                        CHECK_AND_SAVE_OFFSET(last_char_offset - globals.kmer_k, 1, 0);
                    }
                }

                // solid
                key = (edge.data_[0] << 2) >> (kCharsPerEdgeWord - kBucketPrefixLength) * kBitsPerEdgeChar;
                CHECK_AND_SAVE_OFFSET(last_char_offset - globals.kmer_k, 0, 1);

                if (!is_palindrome) {
                    key = (rev_edge.data_[0] << 2) >> (kCharsPerEdgeWord - kBucketPrefixLength) * kBitsPerEdgeChar;
                    CHECK_AND_SAVE_OFFSET(last_char_offset - globals.kmer_k, 1, 1);
                }

                // right $
                if (last_char_offset == read_length - 1 || !globals.is_solid.get(full_offset + 1)) {
                    key = (edge.data_[0] << 4) >> (kCharsPerEdgeWord - kBucketPrefixLength) * kBitsPerEdgeChar;
                    CHECK_AND_SAVE_OFFSET(last_char_offset - globals.kmer_k, 0, 2);
                    if (!is_palindrome) {
                        key = rev_edge.data_[0] >> (kCharsPerEdgeWord - kBucketPrefixLength) * kBitsPerEdgeChar;
                        CHECK_AND_SAVE_OFFSET(last_char_offset - globals.kmer_k, 1, 2);
                    }
                }
            }

            ++full_offset;

            if (++last_char_offset >= read_length) {
                break;
            } else {
                int c = globals.package.get_base(read_id, last_char_offset);
                edge.ShiftAppend(c, globals.kmer_k + 1);
                rev_edge.ShiftPreappend(3 - c, globals.kmer_k + 1);
            }
        }
    }

#undef CHECK_AND_SAVE_OFFSET

    free(prev_full_offsets);
    return NULL;
}

void* s2_lv2_extract_substr(void* _data) {
    bucketpartition_data_t &bp = *((bucketpartition_data_t*) _data);
    read2sdbg_global_t &globals = *(bp.globals);
    int *lv1_p = globals.lv1_items + globals.cx1.rp_[0].rp_bucket_offsets[bp.bp_start_bucket];
    int64_t offset_mask = (1 << globals.offset_num_bits) - 1; // 0000....00011..11
    uint32_t *substrings_p = globals.lv2_substrings +
                             (globals.cx1.rp_[0].rp_bucket_offsets[bp.bp_start_bucket] - globals.cx1.rp_[0].rp_bucket_offsets[globals.cx1.lv2_start_bucket_]);

    for (int b = bp.bp_start_bucket; b < bp.bp_end_bucket; ++b) {
        for (int t = 0; t < globals.num_cpu_threads; ++t) {
            int64_t full_offset = globals.cx1.rp_[t].rp_lv1_differential_base;
            int num = globals.cx1.rp_[t].rp_bucket_sizes[b];
            for (int i = 0; i < num; ++i) {
                if (*lv1_p >= 0) {
                    full_offset += *(lv1_p++);
                } else {
                    full_offset = globals.cx1.lv1_items_special_[-1 - *(lv1_p++)];
                }

                int64_t read_id = full_offset >> (globals.offset_num_bits + 3);
                int offset = (full_offset >> 3) & offset_mask;
                int strand = full_offset & 1;
                int edge_type = (full_offset >> 1) & 3;
                int read_length = globals.package.length(read_id);

                int64_t which_word = globals.package.get_start_index(read_id) / 16;
                int start_offset = globals.package.get_start_index(read_id) % 16;
                uint32_t *read_p = &globals.package.packed_seq[which_word];
                int words_this_read = DivCeiling(start_offset + read_length, 16);

                if (strand == 0) {
                    int num_chars_to_copy = globals.kmer_k;
                    uint8_t prev = kSentinelValue;

                    switch (edge_type) {
                    case 0:
                        break;
                    case 1:
                        prev = globals.package.get_base(read_id, offset);
                        offset++;
                        break;
                    case 2:
                        prev = globals.package.get_base(read_id, offset + 1);
                        offset += 2;
                        num_chars_to_copy--;
                        break;
                    default:
                        assert(false);
                    }
                    CopySubstring(substrings_p, read_p, offset + start_offset, num_chars_to_copy,
                                  globals.cx1.lv2_num_items_, words_this_read, globals.words_per_substring);

                    uint32_t *last_word = substrings_p + int64_t(globals.words_per_substring - 1) * globals.cx1.lv2_num_items_;
                    *last_word |= int(num_chars_to_copy == globals.kmer_k) << kBWTCharNumBits;
                    *last_word |= prev;
                } else {
                    int num_chars_to_copy = globals.kmer_k;
                    uint8_t prev = kSentinelValue;

                    switch (edge_type) {
                    case 0:
                        num_chars_to_copy--;
                        prev = 3 - globals.package.get_base(read_id, offset + globals.kmer_k - 1);
                        break;
                    case 1:
                        prev = 3 - globals.package.get_base(read_id, offset + globals.kmer_k);
                        break;
                    case 2:
                        offset++;
                        break;
                    default:
                        assert(false);
                    }

                    CopySubstringRC(substrings_p, read_p, offset + start_offset, num_chars_to_copy,
                                    globals.cx1.lv2_num_items_, words_this_read, globals.words_per_substring);

                    uint32_t *last_word = substrings_p + int64_t(globals.words_per_substring - 1) * globals.cx1.lv2_num_items_;
                    *last_word |= int(num_chars_to_copy == globals.kmer_k) << kBWTCharNumBits;
                    *last_word |= prev;
                }

                substrings_p++;
            }
        }
    }
    return NULL;
}

void s2_lv2_sort(read2sdbg_global_t &globals) {
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

void s2_lv2_pre_output_partition(read2sdbg_global_t &globals) {
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

    pthread_barrier_init(&globals.output_barrier, NULL, globals.num_output_threads);
}

void* s2_lv2_output(void* _op) {
    xtimer_t local_timer;
    if (cx1_t::kCX1Verbose >= 4) {
        local_timer.reset();
        local_timer.start();
    }
    outputpartition_data_t *op = (outputpartition_data_t*) _op;
    read2sdbg_global_t &globals = *(op->globals);
    int64_t op_start_index = op->op_start_index;
    int64_t op_end_index = op->op_end_index;
    int thread_id = op->op_id;
    int start_idx, end_idx;
    int has_solid_a = 0; // has solid (k+1)-mer aSb
    int has_solid_b = 0; // has solid aSb
    int last_a[4], outputed_b;

    globals.lv2_output_items[thread_id].clear();
    // xlog("%d %d %d\n", thread_id, op_start_index, op_end_index);

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
            uint64_t count = std::min(j - i, kMaxMulti_t);

            if (a == kSentinelValue) {
                assert(b != kSentinelValue);
                if (has_solid_b & (1 << b)) {
                    continue;
                }
                is_dollar = 1;
                count = 0;
            }

            if (b == kSentinelValue) {
                assert(a != kSentinelValue);
                if (has_solid_a & (1 << a)) {
                    continue;
                }
                count = 0;
            }

            w = (b == kSentinelValue) ? 0 : ((outputed_b & (1 << b)) ? b + 5 : b + 1);
            outputed_b |= 1 << b;
            last = (a == kSentinelValue) ? 0 : ((last_a[a] == j - 1) ? 1 : 0);

            // save this item to the out_item array
            assert(i >= 0 && i < (1 << 30) - 1);
            uint64_t out_item = (uint64_t(i) << 32ULL) | (count << 16ULL) | (is_dollar << 5ULL) | (last << 4ULL) | w;
            globals.lv2_output_items[thread_id].push_back(out_item);
        }
    }


    if (cx1_t::kCX1Verbose >= 4) {
        local_timer.stop();
        xlog("SdBG calculation time elapsed: %.4f\n", local_timer.elapsed());
    }

    pthread_barrier_wait(&globals.output_barrier);

    if (op_start_index == 0) {
        if (cx1_t::kCX1Verbose >= 4) {
            local_timer.reset();
            local_timer.start();
        }
        for (int tid = 0; tid < globals.num_output_threads; ++tid) {
            for (std::vector<uint64_t>::const_iterator it = globals.lv2_output_items[tid].begin(); it != globals.lv2_output_items[tid].end(); ++it) {
                int i = (*it) >> 32;
                uint32_t *item = globals.lv2_substrings_db + globals.permutation_db[i];
                while (ExtractFirstChar(item) > globals.cur_suffix_first_char) {
                    ++globals.cur_suffix_first_char;
                    fprintf(globals.output_f_file, "%lld\n", (long long)globals.total_number_edges);
                }

                multi_t counting_db = (*it) >> 16 & kMaxMulti_t;
                // output
                globals.sdbg_writer.outputW(*it & 0xF);
                globals.sdbg_writer.outputLast((*it >> 4) & 1);
                globals.sdbg_writer.outputIsDollar((*it >> 5) & 1);
                if (counting_db <= kMaxMulti2_t) {
                    multi2_t c = counting_db;
                    fwrite(&c, sizeof(multi2_t), 1, globals.output_multiplicity_file);
                } else {
                    int64_t c = counting_db | (globals.total_number_edges << 16);
                    fwrite(&c, sizeof(int64_t), 1, globals.output_multiplicity_file2);
                    fwrite(&kMulti2Sp, sizeof(multi2_t), 1, globals.output_multiplicity_file);
                }

                // stat
                globals.total_number_edges++;
                globals.num_chars_in_w[*it & 0xF]++;
                globals.num_ones_in_last += (*it >> 4) & 1;

                if ((*it >> 5) & 1) {
                    globals.num_dollar_nodes++;
                    if (globals.num_dollar_nodes >= kMaxDummyEdges) {
                        xerr_and_exit("Too many dummy nodes (>= %lld)! The graph contains too many tips!\n", (long long)kMaxDummyEdges);
                    }
                    for (int64_t i = 0; i < globals.words_per_dummy_node; ++i) {
                        globals.dummy_nodes_writer.output(item[i * globals.lv2_num_items_db]);
                    }
                }
                if ((*it & 0xF) == 0) {
                    globals.num_dummy_edges++;
                }
            }
        }

        if (cx1_t::kCX1Verbose >= 4) {
            local_timer.stop();
            xlog("Linear part: %lf\n", local_timer.elapsed());
        }
    }
    return NULL;
}

void s2_lv2_post_output(read2sdbg_global_t &globals) {
    pthread_barrier_destroy(&globals.output_barrier);
}

void s2_post_proc(read2sdbg_global_t &globals) {
    if (cx1_t::kCX1Verbose >= 3) {
        xlog("Number of $ A C G T A- C- G- T-:\n");
        xlog("");
        for (int i = 0; i < 9; ++i) {
            xlog_ext("%lld ", globals.num_chars_in_w[i]);
        }
        xlog_ext("\n");
    }

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

    // --- clean ---
    pthread_mutex_destroy(&globals.lv1_items_scanning_lock);
    free(globals.lv1_items);
    free(globals.lv2_substrings);
    free(globals.permutation);
    free(globals.lv2_substrings_db);
    free(globals.permutation_db);
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

} // s2

} // cx1_read2sdbg