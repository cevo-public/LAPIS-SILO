//
// Created by Alexander Taepper on 26.09.22.
//

#ifndef SILO_SEQUENCE_STORE_H
#define SILO_SEQUENCE_STORE_H

#include "meta_store.h"
#include "silo/roaring/roaring.hh"
#include "silo/roaring/roaring_serialize.h"

namespace silo {

struct Position {
   friend class boost::serialization::access;

   template <class Archive>
   void serialize(Archive& ar, [[maybe_unused]] const unsigned int version) {
      ar& flipped_bitmap;
      ar& bitmaps;
   }

   roaring::Roaring bitmaps[symbolCount];
   // Reference bitmap is flipped
   uint32_t flipped_bitmap = UINT32_MAX;
};

struct CompressedPosition {
   friend class boost::serialization::access;

   template <class Archive>
   void serialize(Archive& ar, [[maybe_unused]] const unsigned int version) {
      ar& flipped_bitmap;
      ar& bitmaps;
   }

   roaring::Roaring bitmaps[symbolCount];
   // Reference bitmap is flipped
   uint32_t flipped_bitmap;
};

class SequenceStore;

class CompressedSequenceStore {
   private:
   unsigned sequence_count;

   public:
   friend class boost::serialization::access;

   template <class Archive>
   void serialize(Archive& ar, [[maybe_unused]] const unsigned int version) {
      ar& sequence_count;
      ar& positions;
      ar& start_gaps;
      ar& end_gaps;
   }

   explicit CompressedSequenceStore(const SequenceStore& seq_store);

   CompressedPosition positions[genomeLength];
   std::vector<uint32_t> start_gaps;
   std::vector<uint32_t> end_gaps;
};

class SequenceStore {
   private:
   unsigned sequence_count;

   public:
   friend class CompressedSequenceStore;
   friend class boost::serialization::access;

   template <class Archive>
   void serialize(Archive& ar, [[maybe_unused]] const unsigned int version) {
      ar& sequence_count;
      ar& positions;
   }
   Position positions[genomeLength];

   [[nodiscard]] size_t computeSize() const {
      size_t result = 0;
      for (auto& p : positions) {
         for (auto& b : p.bitmaps) {
            result += b.getSizeInBytes();
         }
      }
      return result;
   }

   /// default constructor
   SequenceStore() {}

   /// decompress sequence_store
   explicit SequenceStore(const CompressedSequenceStore& c_seq_store);

   /// pos: 1 indexed position of the genome
   [[nodiscard]] const roaring::Roaring* bm(size_t pos, Symbol s) const {
      return &positions[pos - 1].bitmaps[s];
   }

   /// Returns an Roaring-bitmap which has the given residue r at the position pos,
   /// where the residue is interpreted in the _a_pproximate meaning
   /// That means a symbol matches all mixed symbols, which can indicate the residue
   /// pos: 1 indexed position of the genome
   [[nodiscard]] roaring::Roaring* bma(size_t pos, Symbol r) const;

   /// Same as before for flipped bitmaps for r
   [[nodiscard]] roaring::Roaring* bma_neg(size_t pos, Symbol r) const;

   void interpret(const std::vector<std::string>& genomes);

   void interpret_offset_p(const std::vector<std::string>& genomes, uint32_t offset);

   int db_info(std::ostream& io) const;
};

[[maybe_unused]] unsigned save_db(const SequenceStore& db, const std::string& db_filename);

[[maybe_unused]] unsigned load_db(SequenceStore& db, const std::string& db_filename);

[[maybe_unused]] unsigned runOptimize(SequenceStore& db);

[[maybe_unused]] unsigned shrinkToFit(SequenceStore& db);

} //namespace silo;

#endif //SILO_SEQUENCE_STORE_H
