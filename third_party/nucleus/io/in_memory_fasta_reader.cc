/*
 * Copyright 2018 Google LLC.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * 3. Neither the name of the copyright holder nor the names of its
 *    contributors may be used to endorse or promote products derived from this
 *    software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include "third_party/nucleus/io/in_memory_fasta_reader.h"

#include <stddef.h>
#include <utility>

#include "third_party/nucleus/io/reader_base.h"
#include "third_party/nucleus/protos/range.pb.h"
#include "third_party/nucleus/protos/reference.pb.h"
#include "third_party/nucleus/util/utils.h"
#include "tensorflow/core/lib/core/errors.h"

namespace nucleus {

using nucleus::genomics::v1::Range;
using nucleus::genomics::v1::ReferenceSequence;

// Iterable class for traversing all Fasta records in the file.
class FastaFullFileIterable : public GenomeReferenceRecordIterable {
 public:
  // Advance to the next record.
  StatusOr<bool> Next(GenomeReferenceRecord* out) override;

  // Constructor is invoked via InMemoryFastaReader::Iterate.
  FastaFullFileIterable(const InMemoryFastaReader* reader);
  ~FastaFullFileIterable() override;

 private:
  size_t pos_ = 0;
};

// Initializes an InMemoryFastaReader from contigs and seqs.
//
// contigs is a vector describing the "contigs" of this GenomeReference. These
// should include only the contigs present in seqs. A ContigInfo object for a
// contig `chrom` should describe the entire chromosome `chrom` even if the
// corresponding ReferenceSequence only contains a subset of the bases.
//
// seqs is a vector where each element describes a region of the genome we are
// caching in memory and will use to provide bases in the query() operation.
//
// Note that only a single ReferenceSequence for each contig is currently
// supported.
//
// There should be exactly one ContigInfo for each reference_name referred to
// across all ReferenceSequences, and no extra ContigInfos.
StatusOr<std::unique_ptr<InMemoryFastaReader>> InMemoryFastaReader::Create(
    const std::vector<nucleus::genomics::v1::ContigInfo>& contigs,
    const std::vector<nucleus::genomics::v1::ReferenceSequence>& seqs) {
  std::unordered_map<string, nucleus::genomics::v1::ReferenceSequence> seqs_map;

  for (const auto& seq : seqs) {
    if (seq.region().reference_name().empty() || seq.region().start() < 0 ||
        seq.region().start() > seq.region().end()) {
      return tensorflow::errors::InvalidArgument(
          "Malformed region ", seq.region().ShortDebugString());
    }

    const size_t region_len = seq.region().end() - seq.region().start();
    if (region_len != seq.bases().length()) {
      return tensorflow::errors::InvalidArgument(
          "Region size = ", region_len, " not equal to bases.length() ",
          seq.bases().length());
    }

    auto insert_result = seqs_map.emplace(seq.region().reference_name(), seq);
    if (!insert_result.second) {
      return tensorflow::errors::InvalidArgument(
          "Each ReferenceSequence must be on a different chromosome but "
          "multiple ones were found on ",
          seq.region().reference_name());
    }
  }

  return std::unique_ptr<InMemoryFastaReader>(
      new InMemoryFastaReader(contigs, seqs_map));
}

StatusOr<std::shared_ptr<GenomeReferenceRecordIterable>>
InMemoryFastaReader::Iterate() const {
  return StatusOr<std::shared_ptr<GenomeReferenceRecordIterable>>(
      MakeIterable<FastaFullFileIterable>(this));
}

StatusOr<string> InMemoryFastaReader::GetBases(const Range& range) const {
  if (!IsValidInterval(range))
    return tensorflow::errors::InvalidArgument("Invalid interval: ",
                                               range.ShortDebugString());

  const ReferenceSequence& seq = seqs_.at(range.reference_name());

  if (range.start() < seq.region().start() ||
      range.end() > seq.region().end()) {
    return tensorflow::errors::InvalidArgument(
        "Cannot query range=", range.ShortDebugString(),
        " as this InMemoryFastaReader only has bases in the interval=",
        seq.region().ShortDebugString());
  }
  const int64 pos = range.start() - seq.region().start();
  const int64 len = range.end() - range.start();
  return seq.bases().substr(pos, len);
}

StatusOr<bool> FastaFullFileIterable::Next(GenomeReferenceRecord* out) {
  TF_RETURN_IF_ERROR(CheckIsAlive());
  const InMemoryFastaReader* fasta_reader =
      static_cast<const InMemoryFastaReader*>(reader_);
  if (pos_ >= fasta_reader->contigs_.size()) {
    return false;
  }
  const string& reference_name = fasta_reader->contigs_.at(pos_).name();
  auto seq_iter = fasta_reader->seqs_.find(reference_name);
  if (seq_iter == fasta_reader->seqs_.end()) {
    return false;
  }
  DCHECK_NE(nullptr, out) << "FASTA record cannot be null";
  out->first = reference_name;
  out->second = seq_iter->second.bases();
  pos_++;
  return true;
}

FastaFullFileIterable::~FastaFullFileIterable() {}

FastaFullFileIterable::FastaFullFileIterable(const InMemoryFastaReader* reader)
    : Iterable(reader) {}

}  // namespace nucleus
