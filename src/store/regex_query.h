// Required trigrams of a regular expression, after Russ Cox,
// "Regular Expression Matching with a Trigram Index" (Google Code Search).
//
// The analysis covers the PCRE2 syntax subset that matters for search: literals, escapes,
// classes, groups, alternation, quantifiers, anchors and lookarounds (ignored). Anything it
// does not understand (backreferences inside quantifiers, (?x), callouts, ...) makes that
// part of the pattern unconstrained, so the result is always a superset of the matches: an
// all-blocks query means a full scan. Literals are case-folded like the index.
#pragma once

#include <string_view>

#include "store/trigram.h"

namespace kt::store {

trigram::Query regex_trigram_query(std::string_view pattern);

}  // namespace kt::store
