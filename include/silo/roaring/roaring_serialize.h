//
// Created by Alexander Taepper on 09.01.23.
//

#ifndef SILO_ROARING_SERIALIZE_H
#define SILO_ROARING_SERIALIZE_H

#include "roaring.hh"
#include <boost/exception/diagnostic_information.hpp>
#include <boost/iostreams/copy.hpp>
#include <boost/serialization/binary_object.hpp>
#include <boost/serialization/split_member.hpp>
#include <boost/serialization/unordered_map.hpp>
#include <boost/serialization/vector.hpp>

BOOST_SERIALIZATION_SPLIT_FREE(::roaring::Roaring)
namespace boost::serialization {
template <class Archive>
[[maybe_unused]] void save(Archive& ar, const roaring::Roaring& bitmask,
                           [[maybe_unused]] const unsigned int version) {
   std::size_t expected_size_in_bytes = bitmask.getSizeInBytes();
   std::vector<char> buffer(expected_size_in_bytes);
   std::size_t size_in_bytes = bitmask.write(buffer.data());

   ar& size_in_bytes;
   ar& ::boost::serialization::make_binary_object(buffer.data(), size_in_bytes);
}

template <class Archive>
[[maybe_unused]] void load(Archive& ar, roaring::Roaring& bitmask, [[maybe_unused]] const unsigned int version) {
   std::size_t size_in_bytes = 0;
   ar& size_in_bytes;
   std::vector<char> buffer(size_in_bytes);
   ar& ::boost::serialization::make_binary_object(buffer.data(), size_in_bytes);
   bitmask = roaring::Roaring::readSafe(buffer.data(), size_in_bytes);
}
} // namespace boost::serialization;

#endif //SILO_ROARING_SERIALIZE_H
