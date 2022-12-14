//
// Created by Alexander Taepper on 16.11.22.
//

#include <boost/archive/binary_iarchive.hpp>
#include <boost/archive/binary_oarchive.hpp>
#include <silo/common/fix_rh_map.hpp>
#include <syncstream>
#include <silo/common/SizeSketch.h>
#include <silo/common/hashing.h>
#include <silo/common/istream_wrapper.h>
#include <silo/database.h>
#include <tbb/blocked_range.h>
#include <tbb/parallel_for_each.h>

void silo::Database::build(const std::string& part_prefix, const std::string& meta_suffix, const std::string& seq_suffix) {
   partitions.resize(part_def->partitions.size());
   tbb::parallel_for((size_t) 0, part_def->partitions.size(), [&](size_t i) {
      const auto& part = part_def->partitions[i];
      partitions[i].chunks = part.chunks;
      for (unsigned j = 0; j < part.chunks.size(); ++j) {
         std::string name;
         name = part_prefix + chunk_string(i, j);
         std::string seq_file_str = name + seq_suffix;
         std::ifstream meta_in(name + meta_suffix);
         if (!istream_wrapper(seq_file_str).get_is()) {
            seq_file_str += ".xz";
            if (!istream_wrapper(seq_file_str).get_is()) {
               std::osyncstream(std::cerr) << "Sequence_file " << (name + seq_suffix) << " not found" << std::endl;
               return;
            }
            std::osyncstream(std::cerr) << "Using sequence_file " << (seq_file_str) << std::endl;
         } else {
            std::osyncstream(std::cerr) << "Using sequence_file " << (seq_file_str) << std::endl;
         }
         if (!meta_in) {
            std::osyncstream(std::cerr) << "Meta_in file " << (name + meta_suffix) << " not found" << std::endl;
            return;
         }
         silo::istream_wrapper seq_in(seq_file_str);
         std::osyncstream(std::cerr) << "Using meta_in file " << (name + meta_suffix) << std::endl;
         unsigned count1 = processSeq(partitions[i].seq_store, seq_in.get_is());
         unsigned count2 = processMeta(partitions[i].meta_store, meta_in, alias_key, *dict);
         if (count1 != count2) {
            // Fatal error
            std::osyncstream(std::cerr) << "Sequences in meta data and sequence data for chunk " << chunk_string(i, j) << " are not equal." << std::endl;
            std::osyncstream(std::cerr) << "Abort build." << std::endl;
            throw std::runtime_error("Error");
         }
         partitions[i].sequenceCount += count1;
      }
   });
   // Flip Bitmaps of most common symbol, precompute Bitmaps for selected columns.
   finalize();
}

void silo::DatabasePartition::finalize(const Dictionary& dict) {
   std::vector<std::vector<unsigned>> counts_per_pos_per_symbol;
   counts_per_pos_per_symbol.resize(genomeLength);
   for (std::vector<unsigned>& v : counts_per_pos_per_symbol) {
      v.resize(symbolCount);
   }

   for (auto& chunk : chunks) {
      for (auto& pango : chunk.pangos) {
         uint32_t pango_id = dict.get_pangoid(pango);
         if (pango_id != UINT32_MAX && std::find(sorted_lineages.begin(), sorted_lineages.end(), pango_id) != sorted_lineages.end()) {
            sorted_lineages.push_back(pango_id);
         }
      }
   }
   std::sort(sorted_lineages.begin(), sorted_lineages.end());

   tbb::parallel_for((unsigned) 0, genomeLength, [&](unsigned p) {
      unsigned max_symbol = UINT32_MAX;
      unsigned max_count = 0;

      for (unsigned symbol = 0; symbol <= Symbol::N; ++symbol) {
         unsigned count = seq_store.positions[p].bitmaps[symbol].cardinality();
         if (count > max_count) {
            max_symbol = symbol;
            max_count = count;
         }
      }
      if (max_symbol == Symbol::A || max_symbol == Symbol::C || max_symbol == Symbol::G || max_symbol == Symbol::T) {
         seq_store.positions[p].flipped_bitmap = max_symbol;
         seq_store.positions[p].bitmaps[max_symbol].flip(0, sequenceCount);
      }
   });

   { /// Precompute all bitmaps for pango_lineages and -sublineages
      const uint32_t pango_count = dict.get_pango_count();
      std::vector<std::vector<uint32_t>> group_by_lineages(pango_count);
      for (uint32_t sid = 0; sid < sequenceCount; ++sid) {
         const auto lineage = meta_store.sid_to_lineage[sid];
         group_by_lineages[lineage].push_back(sid);
      }

      meta_store.lineage_bitmaps.resize(pango_count);
      for (uint32_t pango = 0; pango < pango_count; ++pango) {
         meta_store.lineage_bitmaps[pango].addMany(group_by_lineages[pango].size(), group_by_lineages[pango].data());
      }

      meta_store.sublineage_bitmaps.resize(pango_count);

      for (uint32_t pango1 = 0; pango1 < pango_count; ++pango1) {
         // Initialize with all lineages that are in pango1
         std::vector<uint32_t> group_by_lineages_sub(group_by_lineages[pango1]);

         // Now add all lineages that I am a prefix of
         for (uint32_t pango2 = 0; pango2 < pango_count; ++pango2) {
            const std::string& str1 = dict.get_pango(pango1);
            const std::string& str2 = dict.get_pango(pango2);
            if (str1.length() >= str2.length()) {
               continue;
            }
            // Check if str1 is a prefix of str2 -> str2 is a sublineage of str1
            if (str2.starts_with(str1)) {
               for (uint32_t pid : group_by_lineages[pango2])
                  group_by_lineages_sub.push_back(pid);
            }
         }
         // Sort, for roaring insert
         std::sort(group_by_lineages_sub.begin(), group_by_lineages_sub.end());
         meta_store.sublineage_bitmaps[pango1].addMany(group_by_lineages_sub.size(), group_by_lineages_sub.data());
      }
   }

   { /// Precompute all bitmaps for countries
      const uint32_t country_count = dict.get_country_count();
      std::vector<std::vector<uint32_t>> group_by_country(country_count);
      for (uint32_t sid = 0; sid < sequenceCount; ++sid) {
         const auto& country = meta_store.sid_to_country[sid];
         group_by_country[country].push_back(sid);
      }

      meta_store.country_bitmaps.resize(country_count);
      for (uint32_t country = 0; country < country_count; ++country) {
         meta_store.country_bitmaps[country].addMany(group_by_country[country].size(), group_by_country[country].data());
      }
   }

   { /// Precompute all bitmaps for regions
      const uint32_t region_count = dict.get_region_count();
      std::vector<std::vector<uint32_t>> group_by_region(region_count);
      for (uint32_t sid = 0; sid < sequenceCount; ++sid) {
         const auto& region = meta_store.sid_to_region[sid];
         group_by_region[region].push_back(sid);
      }

      meta_store.region_bitmaps.resize(region_count);
      for (uint32_t region = 0; region < region_count; ++region) {
         meta_store.region_bitmaps[region].addMany(group_by_region[region].size(), group_by_region[region].data());
      }
   }
}

void silo::Database::finalize() {
   tbb::parallel_for_each(partitions.begin(), partitions.end(), [&](DatabasePartition& p) {
      p.finalize(*dict);
   });
}

/*
void silo::Database::analyse() {
   std::vector<std::vector<unsigned>> counts_per_pos_per_symbol;
   counts_per_pos_per_symbol.resize(genomeLength);
   for (std::vector<unsigned>& v : counts_per_pos_per_symbol) {
      v.resize(symbolCount);
   }

   for (const DatabasePartition& dbp : partitions) {
      tbb::parallel_for((unsigned) 0, genomeLength, [&](unsigned p) {
         for (unsigned symbol = 0; symbol < symbolCount; ++symbol) {
            ++counts_per_pos_per_symbol[p][symbol];
         }
      });
   }
}*/

using r_stat = roaring::api::roaring_statistics_t;

static inline void addStat(r_stat& r1, const r_stat& r2) {
   r1.cardinality += r2.cardinality;
   if (r2.max_value > r1.max_value) r1.max_value = r2.max_value;
   if (r2.min_value < r1.min_value) r1.min_value = r2.min_value;
   r1.n_array_containers += r2.n_array_containers;
   r1.n_run_containers += r2.n_run_containers;
   r1.n_bitset_containers += r2.n_bitset_containers;
   r1.n_bytes_array_containers += r2.n_bytes_array_containers;
   r1.n_bytes_run_containers += r2.n_bytes_run_containers;
   r1.n_bytes_bitset_containers += r2.n_bytes_bitset_containers;
   r1.n_values_array_containers += r2.n_values_array_containers;
   r1.n_values_run_containers += r2.n_values_run_containers;
   r1.n_values_bitset_containers += r2.n_values_bitset_containers;
   r1.n_containers += r2.n_containers;
   r1.sum_value += r2.sum_value;
}

int silo::Database::db_info(std::ostream& io) {
   std::atomic<uint32_t> sequence_count = 0;
   std::atomic<uint64_t> total_size = 0;
   tbb::parallel_for_each(partitions.begin(), partitions.end(), [&](const DatabasePartition& dbp) {
      sequence_count += dbp.sequenceCount;
      total_size += dbp.seq_store.computeSize();
   });

   std::osyncstream(io) << "sequence count: " << number_fmt(sequence_count) << std::endl;
   std::osyncstream(io) << "total size: " << number_fmt(total_size) << std::endl;

   return 0;
}

int silo::Database::db_info_detailed(std::ostream& io) {
   std::vector<size_t> size_by_symbols(symbolCount);

   tbb::parallel_for((unsigned) 0, symbolCount, [&](unsigned symbol) {
      for (const DatabasePartition& dbp : partitions) {
         for (const auto& position : dbp.seq_store.positions) {
            size_by_symbols[symbol] += position.bitmaps[symbol].getSizeInBytes();
         }
      }
   });
   for (unsigned symbol = 0; symbol < symbolCount; ++symbol) {
      io << "size for symbol '" << symbol_rep[symbol] << "': "
         << number_fmt(size_by_symbols[symbol]) << std::endl;
   }

   std::mutex lock;
   std::vector<uint32_t> bitset_containers_by_500pos((genomeLength / 500) + 1);
   std::vector<uint32_t> gap_bitset_containers_by_500pos((genomeLength / 500) + 1);
   std::vector<uint32_t> N_bitset_containers_by_500pos((genomeLength / 500) + 1);
   r_stat s_total{};
   uint64_t total_size_comp = 0;
   uint64_t total_size_frozen = 0;
   /// Because the counters in r_stat are 32 bit and overflow..
   uint64_t n_bytes_array_containers = 0;
   uint64_t n_bytes_run_containers = 0;
   uint64_t n_bytes_bitset_containers = 0;

   tbb::parallel_for((unsigned) 0, genomeLength, [&](unsigned pos) {
      r_stat s_local{};
      uint64_t total_size_comp_local = 0;
      uint64_t total_size_frozen_local = 0;
      uint64_t n_bytes_array_containers_local = 0;
      uint64_t n_bytes_run_containers_local = 0;
      uint64_t n_bytes_bitset_containers_local = 0;
      {
         r_stat s;
         for (const auto& dbp : partitions) {
            const Position& p = dbp.seq_store.positions[pos];
            for (const roaring::Roaring& bm : p.bitmaps) {
               roaring_bitmap_statistics(&bm.roaring, &s);
               addStat(s_local, s);
               total_size_comp_local += bm.getSizeInBytes();
               total_size_frozen_local += bm.getFrozenSizeInBytes();
               n_bytes_array_containers_local += s.n_bytes_array_containers;
               n_bytes_run_containers_local += s.n_bytes_run_containers;
               n_bytes_bitset_containers_local += s.n_bytes_bitset_containers;
               if (s.n_bitset_containers > 0) {
                  if (pos == Symbol::N) {
                     N_bitset_containers_by_500pos[pos / 500] += s.n_bitset_containers;
                  } else if (pos == Symbol::gap) {
                     gap_bitset_containers_by_500pos[pos / 500] += s.n_bitset_containers;
                  } else {
                     bitset_containers_by_500pos[pos / 500] += s.n_bitset_containers;
                  }
               }
            }
         }
      }
      lock.lock();
      addStat(s_total, s_local);
      total_size_comp += total_size_comp_local;
      total_size_frozen += total_size_frozen_local;
      n_bytes_array_containers += n_bytes_array_containers_local;
      n_bytes_run_containers += n_bytes_run_containers_local;
      n_bytes_bitset_containers += n_bytes_bitset_containers_local;
      lock.unlock();
   });
   io << "Total bitmap containers " << number_fmt(s_total.n_containers) << ", of those there are " << std::endl
      << "array: " << number_fmt(s_total.n_array_containers) << std::endl
      << "run: " << number_fmt(s_total.n_run_containers) << std::endl
      << "bitset: " << number_fmt(s_total.n_bitset_containers) << std::endl;
   io << "Total bitmap values " << number_fmt(s_total.cardinality) << ", of those there are " << std::endl
      << "array: " << number_fmt(s_total.n_values_array_containers) << std::endl
      << "run: " << number_fmt(s_total.n_values_run_containers) << std::endl
      << "bitset: " << number_fmt(s_total.n_values_bitset_containers) << std::endl;
   uint64_t total_size = n_bytes_array_containers + n_bytes_run_containers + n_bytes_bitset_containers;

   io << "Total bitmap byte size " << number_fmt(total_size_frozen) << " (frozen) " << std::endl;
   io << "Total bitmap byte size " << number_fmt(total_size_comp) << " (compute_size) " << std::endl;
   io << "Total bitmap byte size " << number_fmt(total_size) << ", of those there are " << std::endl
      << "array: " << number_fmt(s_total.n_bytes_array_containers) << std::endl
      << "run: " << number_fmt(s_total.n_bytes_run_containers) << std::endl
      << "bitset: " << number_fmt(s_total.n_bytes_bitset_containers) << std::endl;

   io << "Bitmap distribution by position #NON_GAP (#GAP)" << std::endl;
   for (unsigned i = 0; i < (genomeLength / 500) + 1; ++i) {
      uint32_t gap_bitsets_at_pos = gap_bitset_containers_by_500pos[i];
      uint32_t N_bitmaps_at_pos = N_bitset_containers_by_500pos[i];
      uint32_t bitmaps_at_pos = bitset_containers_by_500pos[i];
      io << "Pos: [" << i * 500 << "," << ((i + 1) * 500) << "): " << bitmaps_at_pos << " (N: " << N_bitmaps_at_pos << ", -: " << gap_bitsets_at_pos << ")" << '\n';
   }
   io.flush();

   io << "Partition reference genomes: " << std::endl;
   for (const DatabasePartition& dbp : partitions) {
      for (const Position& pos : dbp.seq_store.positions) {
         if(pos.flipped_bitmap == UINT32_MAX){
            io << 'o';
         }
         else{
            io << symbol_rep[pos.flipped_bitmap];
         }
      }
      io << std::endl;
   }

   return 0;
}

unsigned silo::processSeq(silo::SequenceStore& seq_store, std::istream& in) {
   static constexpr unsigned BUFFER_SIZE = 1024;

   unsigned sequence_count = 0;

   std::vector<std::string> genome_buffer;
   while (true) {
      std::string epi_isl, genome;
      if (!getline(in, epi_isl)) {
         break;
      }
      if (!getline(in, genome)) break;
      if (genome.length() != genomeLength) {
         std::cerr << "length mismatch!" << std::endl;
         throw std::runtime_error("length mismatch.");
      }

      genome_buffer.push_back(std::move(genome));
      if (genome_buffer.size() >= BUFFER_SIZE) {
         seq_store.interpret(genome_buffer);
         genome_buffer.clear();
      }

      ++sequence_count;
   }
   seq_store.interpret(genome_buffer);
   seq_store.db_info(std::cout);

   return sequence_count;
}

unsigned silo::processMeta(MetaStore& mdb, std::istream& in, const std::unordered_map<std::string, std::string>& alias_key, const Dictionary& dict) {
   // Ignore header line.
   in.ignore(LONG_MAX, '\n');

   unsigned sequence_count = 0;

   while (true) {
      std::string epi_isl, pango_lineage_raw, date, region, country, division;
      if (!getline(in, epi_isl, '\t')) break;
      if (!getline(in, pango_lineage_raw, '\t')) break;
      if (!getline(in, date, '\t')) break;
      if (!getline(in, region, '\t')) break;
      if (!getline(in, country, '\t')) break;
      if (!getline(in, division, '\n')) break;

      /// Deal with pango_lineage alias:
      std::string pango_lineage = resolve_alias(alias_key, pango_lineage_raw);

      std::string tmp = epi_isl.substr(8);
      uint64_t epi = stoi(tmp);

      struct std::tm tm {};
      std::istringstream ss(date);
      ss >> std::get_time(&tm, "%Y-%m-%d");
      std::time_t time = mktime(&tm);

      std::vector<uint64_t> extra_cols;
      extra_cols.push_back(dict.get_id(division));

      silo::inputSequenceMeta(mdb, epi, time, dict.get_pangoid(pango_lineage),
                              dict.get_regionid(region), dict.get_countryid(country), extra_cols);
      ++sequence_count;
   }

   return sequence_count;
}

void silo::save_pango_defs(const silo::pango_descriptor_t& pd, std::ostream& out) {
   for (auto& x : pd.pangos) {
      out << x.pango_lineage << '\t' << x.count << '\n';
   }
   out.flush();
}

silo::pango_descriptor_t silo::load_pango_defs(std::istream& in) {
   silo::pango_descriptor_t descriptor;
   std::string lineage, count_str;
   uint32_t count;
   while (in && !in.eof()) {
      if (!getline(in, lineage, '\t')) break;
      if (!getline(in, count_str, '\n')) break;
      count = atoi(count_str.c_str());
      descriptor.pangos.emplace_back(silo::pango_t{lineage, count});
   }
   return descriptor;
}

void silo::save_partitioning_descriptor(const silo::partitioning_descriptor_t& pd, std::ostream& out) {
   for (auto& part : pd.partitions) {
      out << "P\t" << part.name << '\t' << part.chunks.size() << '\t' << part.count << '\n';
      for (auto& chunk : part.chunks) {
         out << "C\t" << chunk.prefix << '\t' << chunk.pangos.size() << '\t' << chunk.count << '\t' << chunk.offset << '\n';
         for (auto& pango : chunk.pangos) {
            out << "L\t" << pango << '\n';
         }
      }
   }
}

void silo::Database::save(const std::string& save_dir) {
   if (!part_def) {
      std::cerr << "Cannot save db without part_def." << std::endl;
      return;
   }

   if (pango_def) {
      std::ofstream pango_def_file(save_dir + "pango_def.txt");
      if (!pango_def_file) {
         std::cerr << "Cannot open pango_def output file: " << (save_dir + "pango_def.txt") << std::endl;
         return;
      }
      save_pango_defs(*pango_def, pango_def_file);
   }
   {
      std::ofstream part_def_file(save_dir + "part_def.txt");
      if (!part_def_file) {
         std::cerr << "Cannot open part_def output file: " << (save_dir + "part_def.txt") << std::endl;
         return;
      }
      save_partitioning_descriptor(*part_def, part_def_file);
   }
   {
      std::ofstream dict_output(save_dir + "dict.txt");
      if (!dict_output) {
         std::cerr << "Could not open '" << (save_dir + "dict.txt") << "'." << std::endl;
         return;
      }
      dict->save_dict(dict_output);
   }

   std::vector<std::ofstream> file_vec;
   for (unsigned i = 0; i < part_def->partitions.size(); ++i) {
      file_vec.push_back(std::ofstream(save_dir + 'P' + std::to_string(i) + ".silo"));

      if (!file_vec.back()) {
         std::cerr << "Cannot open partition output file for saving: " << (save_dir + 'P' + std::to_string(i) + ".silo") << std::endl;
         return;
      }
   }

   tbb::parallel_for((size_t) 0, part_def->partitions.size(), [&](size_t i) {
      ::boost::archive::binary_oarchive oa(file_vec[i]);
      oa << partitions[i];
   });
}

void silo::Database::load(const std::string& save_dir) {
   std::ifstream part_def_file(save_dir + "part_def.txt");
   if (!part_def_file) {
      std::cerr << "Cannot open part_def input file for loading: " << (save_dir + "part_def.txt") << std::endl;
      return;
   }
   part_def = std::make_unique<partitioning_descriptor_t>(load_partitioning_descriptor(part_def_file));

   std::ifstream pango_def_file(save_dir + "pango_def.txt");
   if (pango_def_file) {
      pango_def = std::make_unique<pango_descriptor_t>(load_pango_defs(pango_def_file));
   }

   {
      auto dict_input = std::ifstream(save_dir + "dict.txt");
      if (!dict_input) {
         std::cerr << "dict_input file " << (save_dir + "dict.txt") << " not found." << std::endl;
         return;
      }
      std::cout << "Load dictionary from input file " << (save_dir + "dict.txt") << std::endl;
      dict = std::make_unique<Dictionary>(Dictionary::load_dict(dict_input));
   }

   std::cout << "Loading partitions from " << save_dir << std::endl;
   std::vector<std::ifstream> file_vec;
   for (unsigned i = 0; i < part_def->partitions.size(); ++i) {
      file_vec.push_back(std::ifstream(save_dir + 'P' + std::to_string(i) + ".silo"));

      if (!file_vec.back()) {
         std::cerr << "Cannot open partition input file for loading: " << (save_dir + 'P' + std::to_string(i) + ".silo") << std::endl;
         return;
      }
   }

   partitions.resize(part_def->partitions.size());
   tbb::parallel_for((size_t) 0, part_def->partitions.size(), [&](size_t i) {
      ::boost::archive::binary_iarchive ia(file_vec[i]);
      ia >> partitions[i];
   });
}