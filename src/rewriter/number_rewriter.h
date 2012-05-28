// Copyright 2010-2012, Google Inc.
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
//     * Redistributions of source code must retain the above copyright
// notice, this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above
// copyright notice, this list of conditions and the following disclaimer
// in the documentation and/or other materials provided with the
// distribution.
//     * Neither the name of Google Inc. nor the names of its
// contributors may be used to endorse or promote products derived from
// this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#ifndef MOZC_REWRITER_NUMBER_REWRITER_H_
#define MOZC_REWRITER_NUMBER_REWRITER_H_

#include "converter/segments.h"
#include "rewriter/rewriter_interface.h"

namespace mozc {
class ConversionRequest;
class POSMatcher;
struct RewriteCandidateInfo;

class NumberRewriter : public RewriterInterface  {
 public:
  // Rewrite type
  enum RewriteType {
    NO_REWRITE = 0,
    ARABIC_FIRST,  // arabic candidates first ordering
    KANJI_FIRST,  // kanji candidates first ordering
  };

  explicit NumberRewriter(const POSMatcher *pos_matcher);
  virtual ~NumberRewriter();

  virtual int capability() const;

  virtual bool Rewrite(const ConversionRequest &request,
                       Segments *segments) const;

 private:
  bool RewriteOneSegment(bool exec_radix_conversion, Segment *seg) const;
  void GetRewriteCandidateInfos(const Segment &seg,
                                vector<RewriteCandidateInfo>
                                *rewrite_candidate_info) const;

  RewriteType GetRewriteTypeAndBase(
      const Segment &seg,
      int base_candidate_pos,
      Segment::Candidate *arabic_candidate) const;

  bool IsNumber(uint16 lid) const;

  const POSMatcher *pos_matcher_;
};

}  // namespace mozc
#endif  // MOZC_REWRITER_NUMBER_REWRITER_H_
