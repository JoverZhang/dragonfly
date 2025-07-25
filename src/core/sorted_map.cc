// Copyright 2023, Roman Gershman.  All rights reserved.
// See LICENSE for licensing terms.
//

#include "core/sorted_map.h"

#include <absl/strings/str_cat.h>

#include <cmath>

extern "C" {
#include "redis/listpack.h"
#include "redis/redis_aux.h"
#include "redis/util.h"
#include "redis/zmalloc.h"
}

#include <double-conversion/double-to-string.h>

#include "base/endian.h"
#include "base/logging.h"

using namespace std;

namespace dfly {
namespace detail {

namespace {

double GetObjScore(const void* obj) {
  sds s = (sds)obj;
  char* ptr = s + sdslen(s) + 1;
  return absl::bit_cast<double>(absl::little_endian::Load64(ptr));
}

void SetObjScore(void* obj, double score) {
  sds s = (sds)obj;
  char* ptr = s + sdslen(s) + 1;
  absl::little_endian::Store64(ptr, absl::bit_cast<uint64_t>(score));
}

// buf must be at least 10 chars long.
void* BuildScoredKey(double score, char buf[]) {
  buf[0] = SDS_TYPE_5;  // length 0.
  buf[1] = 0;
  absl::little_endian::Store64(buf + 2, absl::bit_cast<uint64_t>(score));
  void* key = buf + 1;

  return key;
}

// Copied from t_zset.c
/* Returns 1 if the double value can safely be represented in long long without
 * precision loss, in which case the corresponding long long is stored in the out variable. */
static int double2ll(double d, long long* out) {
#if (DBL_MANT_DIG >= 52) && (DBL_MANT_DIG <= 63) && (LLONG_MAX == 0x7fffffffffffffffLL)
  /* Check if the float is in a safe range to be casted into a
   * long long. We are assuming that long long is 64 bit here.
   * Also we are assuming that there are no implementations around where
   * double has precision < 52 bit.
   *
   * Under this assumptions we test if a double is inside a range
   * where casting to long long is safe. Then using two castings we
   * make sure the decimal part is zero. If all this is true we can use
   * integer without precision loss.
   *
   * Note that numbers above 2^52 and below 2^63 use all the fraction bits as real part,
   * and the exponent bits are positive, which means the "decimal" part must be 0.
   * i.e. all double values in that range are representable as a long without precision loss,
   * but not all long values in that range can be represented as a double.
   * we only care about the first part here. */
  if (d < (double)(-LLONG_MAX / 2) || d > (double)(LLONG_MAX / 2))
    return 0;
  long long ll = d;
  if (ll == d) {
    *out = ll;
    return 1;
  }
#endif
  return 0;
}

/* Compare element in sorted set with given element. */
int zzlCompareElements(unsigned char* eptr, unsigned char* cstr, unsigned int clen) {
  unsigned char* vstr;
  unsigned int vlen;
  long long vlong;
  unsigned char vbuf[32];
  int minlen, cmp;

  vstr = lpGetValue(eptr, &vlen, &vlong);
  if (vstr == NULL) {
    /* Store string representation of long long in buf. */
    vlen = ll2string((char*)vbuf, sizeof(vbuf), vlong);
    vstr = vbuf;
  }

  minlen = (vlen < clen) ? vlen : clen;
  cmp = memcmp(vstr, cstr, minlen);
  if (cmp == 0)
    return vlen - clen;
  return cmp;
}

using double_conversion::DoubleToStringConverter;
constexpr unsigned kConvFlags = DoubleToStringConverter::UNIQUE_ZERO;

DoubleToStringConverter score_conv(kConvFlags, "inf", "nan", 'e', -6, 21, 6, 0);

// Copied from redis code but uses double_conversion to encode double values.
unsigned char* ZzlInsertAt(unsigned char* zl, unsigned char* eptr, std::string_view ele,
                           double score) {
  unsigned char* sptr;
  char scorebuf[128];
  unsigned scorelen = 0;
  long long lscore;
  int score_is_long = double2ll(score, &lscore);
  if (!score_is_long) {
    // Use double converter to get the shortest representation.
    double_conversion::StringBuilder sb(scorebuf, sizeof(scorebuf));
    score_conv.ToShortest(score, &sb);
    scorelen = sb.position();
    sb.Finalize();
    DCHECK_EQ(scorelen, strlen(scorebuf));
  }

  // Argument parsing converts empty strings to default initialized string views.
  // Such string views have a null data field, which if passed into lpAppend (via zzlInsertAt)
  // results in the replace operation being applied on the listpack. In addition to being wrong, it
  // also causes assertion failures. To circumvent this corner case we pass here a string view
  // pointing to an empty string on the stack, which has a non-null data field.
  if (ele.data() == nullptr) {
    ele = ""sv;
  }

  if (eptr == NULL) {
    zl = lpAppend(zl, (const unsigned char*)(ele.data()), ele.size());
    if (score_is_long)
      zl = lpAppendInteger(zl, lscore);
    else
      zl = lpAppend(zl, (unsigned char*)scorebuf, scorelen);
  } else {
    /* Insert member before the element 'eptr'. */
    zl = lpInsertString(zl, (const unsigned char*)ele.data(), ele.size(), eptr, LP_BEFORE, &sptr);

    /* Insert score after the member. */
    if (score_is_long)
      zl = lpInsertInteger(zl, lscore, sptr, LP_AFTER, NULL);
    else
      zl = lpInsertString(zl, (unsigned char*)scorebuf, scorelen, sptr, LP_AFTER, NULL);
  }
  return zl;
}

double ZzlStrtod(unsigned char* vstr, unsigned int vlen) {
  char buf[128];
  if (vlen > sizeof(buf))
    vlen = sizeof(buf);
  memcpy(buf, vstr, vlen);
  buf[vlen] = '\0';
  return strtod(buf, NULL);
}

/* Return a listpack element as an SDS string. */
sds LpGetObject(const uint8_t* sptr) {
  unsigned char* vstr;
  unsigned int vlen;
  long long vlong;

  serverAssert(sptr != NULL);
  vstr = lpGetValue(const_cast<uint8_t*>(sptr), &vlen, &vlong);

  if (vstr) {
    return sdsnewlen((char*)vstr, vlen);
  } else {
    return sdsfromlonglong(vlong);
  }
}

// static representation of sds strings
char kMinStrData[] =
    "\110"
    "minstring";
char kMaxStrData[] =
    "\110"
    "maxstring";

}  // namespace

double ZzlGetScore(const uint8_t* sptr) {
  unsigned char* vstr;
  unsigned int vlen;
  long long vlong;
  double score;

  DCHECK(sptr != NULL);
  vstr = lpGetValue(const_cast<uint8_t*>(sptr), &vlen, &vlong);

  if (vstr) {
    score = ZzlStrtod(vstr, vlen);
  } else {
    score = vlong;
  }

  return score;
}

/* Move to the previous entry based on the values in eptr and sptr. Both are
 * set to NULL when there is no prev entry. */
void ZzlPrev(const uint8_t* zl, uint8_t** eptr, uint8_t** sptr) {
  unsigned char *_eptr, *_sptr;
  serverAssert(*eptr != NULL && *sptr != NULL);

  _sptr = lpPrev(const_cast<uint8_t*>(zl), *eptr);
  if (_sptr != NULL) {
    _eptr = lpPrev(const_cast<uint8_t*>(zl), _sptr);
    DCHECK(_eptr != NULL);
  } else {
    /* No previous entry. */
    _eptr = NULL;
  }

  *eptr = _eptr;
  *sptr = _sptr;
}

/* Move to next entry based on the values in eptr and sptr. Both are set to
 * NULL when there is no next entry. */
void ZzlNext(const uint8_t* zl, uint8_t** eptr, uint8_t** sptr) {
  unsigned char *_eptr, *_sptr;
  DCHECK(*eptr != NULL && *sptr != NULL);

  _eptr = lpNext(const_cast<uint8_t*>(zl), *sptr);
  if (_eptr != NULL) {
    _sptr = lpNext(const_cast<uint8_t*>(zl), _eptr);
    DCHECK(_sptr != NULL);
  } else {
    /* No next entry. */
    _sptr = NULL;
  }

  *eptr = _eptr;
  *sptr = _sptr;
}

/* Free a lex range structure, must be called only after zslParseLexRange()
 * populated the structure with success (C_OK returned). */
void ZslFreeLexRange(const zlexrangespec* spec) {
  if (spec->min != cminstring && spec->min != cmaxstring)
    sdsfree(spec->min);
  if (spec->max != cminstring && spec->max != cmaxstring)
    sdsfree(spec->max);
}

/* This is just a wrapper to sdscmp() that is able to
 * handle shared.minstring and shared.maxstring as the equivalent of
 * -inf and +inf for strings */
int sdscmplex(sds a, sds b) {
  if (a == b)
    return 0;
  if (a == cminstring || b == cmaxstring)
    return -1;
  if (a == cmaxstring || b == cminstring)
    return 1;
  return sdscmp(a, b);
}

int zslLexValueGteMin(sds value, const zlexrangespec* spec) {
  return spec->minex ? (sdscmplex(value, spec->min) > 0) : (sdscmplex(value, spec->min) >= 0);
}

int zslLexValueLteMax(sds value, const zlexrangespec* spec) {
  return spec->maxex ? (sdscmplex(value, spec->max) < 0) : (sdscmplex(value, spec->max) <= 0);
}

int ZzlLexValueGteMin(unsigned char* p, const zlexrangespec* spec) {
  sds value = LpGetObject(p);
  int res = zslLexValueGteMin(value, spec);
  sdsfree(value);
  return res;
}

int ZzlLexValueLteMax(unsigned char* p, const zlexrangespec* spec) {
  sds value = LpGetObject(p);
  int res = zslLexValueLteMax(value, spec);
  sdsfree(value);
  return res;
}

/* Returns if there is a part of the zset is in range. Should only be used
 * internally by zzlFirstInRange and zzlLastInRange. */
int zzlIsInRange(unsigned char* zl, const zrangespec* range) {
  unsigned char* p;
  double score;

  /* Test for ranges that will always be empty. */
  if (range->min > range->max || (range->min == range->max && (range->minex || range->maxex)))
    return 0;

  p = lpSeek(zl, -1); /* Last score. */
  if (p == NULL)
    return 0; /* Empty sorted set */
  score = ZzlGetScore(p);
  if (!ZslValueGteMin(score, range))
    return 0;

  p = lpSeek(zl, 1); /* First score. */
  serverAssert(p != NULL);
  score = ZzlGetScore(p);
  if (!ZslValueLteMax(score, range))
    return 0;

  return 1;
}

/* Find pointer to the first element contained in the specified range.
 * Returns NULL when no element is contained in the range. */
unsigned char* ZzlFirstInRange(unsigned char* zl, const zrangespec* range) {
  unsigned char *eptr = lpSeek(zl, 0), *sptr;
  double score;

  /* If everything is out of range, return early. */
  if (!zzlIsInRange(zl, range))
    return NULL;

  while (eptr != NULL) {
    sptr = lpNext(zl, eptr);
    serverAssert(sptr != NULL);

    score = ZzlGetScore(sptr);
    if (ZslValueGteMin(score, range)) {
      /* Check if score <= max. */
      if (ZslValueLteMax(score, range))
        return eptr;
      return NULL;
    }

    /* Move to next element. */
    eptr = lpNext(zl, sptr);
  }

  return NULL;
}

/* Find pointer to the last element contained in the specified range.
 * Returns NULL when no element is contained in the range. */
unsigned char* ZzlLastInRange(unsigned char* zl, const zrangespec* range) {
  unsigned char *eptr = lpSeek(zl, -2), *sptr;
  double score;

  /* If everything is out of range, return early. */
  if (!zzlIsInRange(zl, range))
    return NULL;

  while (eptr != NULL) {
    sptr = lpNext(zl, eptr);
    serverAssert(sptr != NULL);

    score = ZzlGetScore(sptr);
    if (ZslValueLteMax(score, range)) {
      /* Check if score >= min. */
      if (ZslValueGteMin(score, range))
        return eptr;
      return NULL;
    }

    /* Move to previous element by moving to the score of previous element.
     * When this returns NULL, we know there also is no element. */
    sptr = lpPrev(zl, eptr);
    if (sptr != NULL)
      serverAssert((eptr = lpPrev(zl, sptr)) != NULL);
    else
      eptr = NULL;
  }

  return NULL;
}

/* Returns if there is a part of the zset is in range. Should only be used
 * internally by zzlFirstInRange and zzlLastInRange. */
int ZzlIsInLexRange(unsigned char* zl, const zlexrangespec* range) {
  unsigned char* p;

  /* Test for ranges that will always be empty. */
  int cmp = sdscmplex(range->min, range->max);
  if (cmp > 0 || (cmp == 0 && (range->minex || range->maxex)))
    return 0;

  p = lpSeek(zl, -2); /* Last element. */
  if (p == NULL)
    return 0;
  if (!ZzlLexValueGteMin(p, range))
    return 0;

  p = lpSeek(zl, 0); /* First element. */
  serverAssert(p != NULL);
  if (!ZzlLexValueLteMax(p, range))
    return 0;

  return 1;
}

/* Find pointer to the first element contained in the specified lex range.
 * Returns NULL when no element is contained in the range. */
unsigned char* ZzlFirstInLexRange(unsigned char* zl, const zlexrangespec* range) {
  unsigned char *eptr = lpSeek(zl, 0), *sptr;

  /* If everything is out of range, return early. */
  if (!ZzlIsInLexRange(zl, range))
    return NULL;

  while (eptr != NULL) {
    if (ZzlLexValueGteMin(eptr, range)) {
      /* Check if score <= max. */
      if (ZzlLexValueLteMax(eptr, range))
        return eptr;
      return NULL;
    }

    /* Move to next element. */
    sptr = lpNext(zl, eptr); /* This element score. Skip it. */
    serverAssert(sptr != NULL);
    eptr = lpNext(zl, sptr); /* Next element. */
  }

  return NULL;
}

/* Find pointer to the last element contained in the specified lex range.
 * Returns NULL when no element is contained in the range. */
unsigned char* ZzlLastInLexRange(unsigned char* zl, const zlexrangespec* range) {
  unsigned char *eptr = lpSeek(zl, -2), *sptr;

  /* If everything is out of range, return early. */
  if (!ZzlIsInLexRange(zl, range))
    return NULL;

  while (eptr != NULL) {
    if (ZzlLexValueLteMax(eptr, range)) {
      /* Check if score >= min. */
      if (ZzlLexValueGteMin(eptr, range))
        return eptr;
      return NULL;
    }

    /* Move to previous element by moving to the score of previous element.
     * When this returns NULL, we know there also is no element. */
    sptr = lpPrev(zl, eptr);
    if (sptr != NULL)
      serverAssert((eptr = lpPrev(zl, sptr)) != NULL);
    else
      eptr = NULL;
  }

  return NULL;
}

unsigned char* ZzlDeleteRangeByLex(unsigned char* zl, const zlexrangespec* range,
                                   unsigned long* deleted) {
  unsigned char *eptr, *sptr;
  unsigned long num = 0;

  if (deleted != NULL)
    *deleted = 0;

  eptr = ZzlFirstInLexRange(zl, range);
  if (eptr == NULL)
    return zl;

  /* When the tail of the listpack is deleted, eptr will be NULL. */
  while (eptr && (sptr = lpNext(zl, eptr)) != NULL) {
    if (ZzlLexValueLteMax(eptr, range)) {
      /* Delete both the element and the score. */
      zl = lpDeleteRangeWithEntry(zl, &eptr, 2);
      num++;
    } else {
      /* No longer in range. */
      break;
    }
  }

  if (deleted != NULL)
    *deleted = num;
  return zl;
}

unsigned char* ZzlDeleteRangeByScore(unsigned char* zl, const zrangespec* range,
                                     unsigned long* deleted) {
  unsigned char *eptr, *sptr;
  double score;
  unsigned long num = 0;

  if (deleted != NULL)
    *deleted = 0;

  eptr = ZzlFirstInRange(zl, range);
  if (eptr == NULL)
    return zl;

  /* When the tail of the listpack is deleted, eptr will be NULL. */
  while (eptr && (sptr = lpNext(zl, eptr)) != NULL) {
    score = ZzlGetScore(sptr);
    if (ZslValueLteMax(score, range)) {
      /* Delete both the element and the score. */
      zl = lpDeleteRangeWithEntry(zl, &eptr, 2);
      num++;
    } else {
      /* No longer in range. */
      break;
    }
  }

  if (deleted != NULL)
    *deleted = num;
  return zl;
}

/* Insert (element,score) pair in listpack. This function assumes the element is
 * not yet present in the list. */
unsigned char* ZzlInsert(unsigned char* zl, std::string_view ele, double score) {
  unsigned char *eptr = NULL, *sptr = lpSeek(zl, -1);
  double s;

  // Optimization: check first whether the new element should be the last.
  if (sptr != NULL) {
    s = ZzlGetScore(sptr);
    if (s >= score) {
      // It should not be the last, so fallback to the forward iteration.
      eptr = lpSeek(zl, 0);
    }
  }

  while (eptr != NULL) {
    sptr = lpNext(zl, eptr);
    s = ZzlGetScore(sptr);

    if (s > score) {
      /* First element with score larger than score for element to be
       * inserted. This means we should take its spot in the list to
       * maintain ordering. */
      return ZzlInsertAt(zl, eptr, ele, score);
    } else if (s == score) {
      /* Ensure lexicographical ordering for elements. */
      if (zzlCompareElements(eptr, (unsigned char*)ele.data(), ele.size()) > 0) {
        return ZzlInsertAt(zl, eptr, ele, score);
      }
    }

    /* Move to next element. */
    eptr = lpNext(zl, sptr);
  }

  /* Push on tail of list when it was not yet inserted. */
  return ZzlInsertAt(zl, NULL, ele, score);
}

unsigned char* ZzlFind(unsigned char* lp, std::string_view ele, double* score) {
  uint8_t *sptr, *eptr = lpFirst(lp);

  if (eptr == nullptr)
    return nullptr;
  eptr = lpFind(lp, eptr, (unsigned char*)ele.data(), ele.size(), 1);
  if (eptr) {
    sptr = lpNext(lp, eptr);
    serverAssert(sptr != NULL);

    /* Matching element, pull out score. */
    if (score != nullptr)
      *score = ZzlGetScore(sptr);
    return eptr;
  }

  return nullptr;
}

SortedMap::SortedMap(PMR_NS::memory_resource* mr)
    : score_map(new ScoreMap(mr)), score_tree(new ScoreTree(mr)) {
}

SortedMap::~SortedMap() {
  delete score_tree;
  delete score_map;
}

// Three way comparison of q and key.
// Compares scores first and then the keys, unless q.ignore_score is set.
// In that case only keys are compared.
// In order to support close/open intervals, we introduce a special flag for +inf strings.
// So, in case of score equality (or if scores are ignored), q.str_is_infinite means q > key,
// and 1 is returned.
int SortedMap::ScoreSdsPolicy::KeyCompareTo::operator()(Query q, ScoreSds key) const {
  sds sdsa = (sds)q.item;

  if (!q.ignore_score) {
    double sa = GetObjScore(sdsa);
    double sb = GetObjScore(key);

    if (sa < sb)
      return -1;
    if (sa > sb)
      return 1;
  }

  // if q.str_is_infinite is set, it means q > key at this point.
  if (q.str_is_infinite)
    return 1;

  return sdscmp(sdsa, (sds)key);
}

int SortedMap::AddElem(double score, std::string_view ele, int in_flags, int* out_flags,
                       double* newscore) {
  // does not take ownership over ele.
  DCHECK(!isnan(score));

  ScoreSds obj = nullptr;
  bool added = false;

  if (in_flags & ZADD_IN_XX) {
    obj = score_map->FindObj(ele);
    if (obj == nullptr) {
      *out_flags = ZADD_OUT_NOP;
      return 1;
    }
  } else {
    tie(obj, added) = score_map->AddOrSkip(ele, score);
  }

  if (added) {
    // Adding a new element.
    DCHECK_EQ(in_flags & ZADD_IN_XX, 0);

    *out_flags = ZADD_OUT_ADDED;
    *newscore = score;
    bool added = score_tree->Insert(obj);
    DCHECK(added);

    return 1;
  }

  // Updating an existing element.
  if ((in_flags & ZADD_IN_NX)) {
    // Updating an existing element.
    *out_flags = ZADD_OUT_NOP;
    return 1;
  }

  if (in_flags & ZADD_IN_INCR) {
    score += GetObjScore(obj);
    if (isnan(score)) {
      *out_flags = ZADD_OUT_NAN;
      return 0;
    }
  }

  // Update the score.
  CHECK(score_tree->Delete(obj));
  SetObjScore(obj, score);
  CHECK(score_tree->Insert(obj));
  *out_flags = ZADD_OUT_UPDATED;
  *newscore = score;
  return 1;
}

optional<double> SortedMap::GetScore(std::string_view ele) const {
  ScoreSds obj = score_map->FindObj(ele);
  if (obj != nullptr) {
    return GetObjScore(obj);
  }

  return std::nullopt;
}

bool SortedMap::InsertNew(double score, std::string_view member) {
  DVLOG(2) << "InsertNew " << score << " " << member;

  auto [newk, added] = score_map->AddOrSkip(member, score);
  if (!added)
    return false;

  added = score_tree->Insert(newk);
  CHECK(added);
  return true;
}

optional<unsigned> SortedMap::GetRank(std::string_view ele, bool reverse) const {
  ScoreSds obj = score_map->FindObj(ele);
  if (obj == nullptr)
    return std::nullopt;

  optional rank = score_tree->GetRank(obj, reverse);
  DCHECK(rank);
  return *rank;
}

SortedMap::ScoredArray SortedMap::GetRange(const zrangespec& range, unsigned offset, unsigned limit,
                                           bool reverse) const {
  ScoredArray arr;
  if (score_tree->Size() <= offset || limit == 0)
    return arr;

  char buf[16];
  if (reverse) {
    ScoreSds key = BuildScoredKey(range.max, buf);
    auto path = score_tree->LEQ(Query{key, false, !range.maxex});
    if (path.Empty())
      return arr;

    if (range.maxex && range.max == GetObjScore(path.Terminal())) {
      ++offset;
    }
    DCHECK_LE(GetObjScore(path.Terminal()), range.max);

    while (offset--) {
      if (!path.Prev())
        return arr;
    }

    while (limit--) {
      ScoreSds ele = path.Terminal();

      double score = GetObjScore(ele);
      if (range.min > score || (range.min == score && range.minex))
        break;
      arr.emplace_back(string{(sds)ele, sdslen((sds)ele)}, score);
      if (!path.Prev())
        break;
    }
  } else {
    ScoreSds key = BuildScoredKey(range.min, buf);
    auto path = score_tree->GEQ(Query{key, false, range.minex});
    if (path.Empty())
      return arr;

    while (offset--) {
      if (!path.Next())
        return arr;
    }

    auto path2 = path;
    size_t num_elems = 0;

    // Count the number of elements in the range.
    while (limit--) {
      ScoreSds ele = path.Terminal();

      double score = GetObjScore(ele);
      if (range.max < score || (range.max == score && range.maxex))
        break;
      ++num_elems;
      if (!path.Next())
        break;
    }

    // reserve enough space.
    arr.resize(num_elems);
    for (size_t i = 0; i < num_elems; ++i) {
      ScoreSds ele = path2.Terminal();
      arr[i] = {string{(sds)ele, sdslen((sds)ele)}, GetObjScore(ele)};
      path2.Next();
    }
  }

  return arr;
}

SortedMap::ScoredArray SortedMap::GetLexRange(const zlexrangespec& range, unsigned offset,
                                              unsigned limit, bool reverse) const {
  if (score_tree->Size() <= offset || limit == 0)
    return {};

  detail::BPTreePath<ScoreSds> path;
  ScoredArray arr;

  if (reverse) {
    if (range.max != cmaxstring) {
      path = score_tree->LEQ(Query{range.max, true});
      if (path.Empty())
        return {};

      if (range.maxex && sdscmp((sds)path.Terminal(), range.max) == 0) {
        ++offset;
      }
      while (offset--) {
        if (!path.Prev())
          return {};
      }
    } else {
      path = score_tree->FromRank(score_tree->Size() - offset - 1);
    }

    while (limit--) {
      ScoreSds ele = path.Terminal();

      if (range.min != cminstring) {
        int cmp = sdscmp((sds)ele, range.min);
        if (cmp < 0 || (cmp == 0 && range.minex))
          break;
      }
      arr.emplace_back(string{(sds)ele, sdslen((sds)ele)}, GetObjScore(ele));
      if (!path.Prev())
        break;
    }
  } else {
    if (range.min != cminstring) {
      path = score_tree->GEQ(Query{range.min, true});
      if (path.Empty())
        return {};

      if (range.minex && sdscmp((sds)path.Terminal(), range.min) == 0) {
        ++offset;
      }
      while (offset--) {
        if (!path.Next())
          return {};
      }
    } else {
      path = score_tree->FromRank(offset);
    }

    while (limit--) {
      ScoreSds ele = path.Terminal();

      if (range.max != cmaxstring) {
        int cmp = sdscmp((sds)ele, range.max);
        if (cmp > 0 || (cmp == 0 && range.maxex))
          break;
      }
      arr.emplace_back(string{(sds)ele, sdslen((sds)ele)}, GetObjScore(ele));
      if (!path.Next())
        break;
    }
  }
  return arr;
}

uint8_t* SortedMap::ToListPack() const {
  uint8_t* lp = lpNew(0);

  score_tree->Iterate(0, UINT32_MAX, [&](ScoreSds ele) {
    const std::string_view v{(sds)ele, sdslen((sds)ele)};
    lp = ZzlInsertAt(lp, NULL, v, GetObjScore(ele));
    return true;
  });

  return lp;
}

bool SortedMap::Delete(std::string_view ele) const {
  ScoreSds obj = score_map->FindObj(ele);
  if (obj == nullptr)
    return false;

  CHECK(score_tree->Delete(obj));
  CHECK(score_map->Erase(ele));
  return true;
}

size_t SortedMap::MallocSize() const {
  // TODO: add malloc used to BPTree.
  return score_map->SetMallocUsed() + score_map->ObjMallocUsed() + score_tree->NodeCount() * 256;
}

bool SortedMap::Reserve(size_t sz) {
  score_map->Reserve(sz);
  return true;
}

size_t SortedMap::DeleteRangeByRank(unsigned start, unsigned end) {
  DCHECK_LE(start, end);
  DCHECK_LT(end, score_tree->Size());

  for (uint32_t i = start; i <= end; ++i) {
    /* Ideally, we would want to advance path to the next item and delete the previous one.
     * However, we can not do that because the path is invalidated after the
     * deletion. So we have to recreate the path for each item using the same rank.
     * Note, it is probably could be improved, but it's much more complicated.
     */

    auto path = score_tree->FromRank(start);
    sds ele = (sds)path.Terminal();
    score_tree->Delete(path);
    score_map->Erase(ele);
  }

  return end - start + 1;
}

size_t SortedMap::DeleteRangeByScore(const zrangespec& range) {
  char buf[16] = {0};
  size_t deleted = 0;

  while (!score_tree->Empty()) {
    ScoreSds min_key = BuildScoredKey(range.min, buf);
    auto path = score_tree->GEQ(Query{min_key, false, range.minex});
    if (path.Empty())
      break;

    ScoreSds item = path.Terminal();
    double score = GetObjScore(item);

    if (range.minex) {
      DCHECK_GT(score, range.min);
    } else {
      DCHECK_GE(score, range.min);
    }
    if (score > range.max || (range.maxex && score == range.max))
      break;

    score_tree->Delete(item);
    ++deleted;
    score_map->Erase((sds)item);
  }

  return deleted;
}

size_t SortedMap::DeleteRangeByLex(const zlexrangespec& range) {
  if (score_tree->Size() == 0)
    return 0;

  size_t deleted = 0;

  uint32_t rank = 0;
  if (range.min != cminstring) {
    auto path = score_tree->GEQ(Query{range.min, true});
    if (path.Empty())
      return {};

    rank = path.Rank();
    if (range.minex && sdscmp((sds)path.Terminal(), range.min) == 0) {
      ++rank;
    }
  }

  while (rank < score_tree->Size()) {
    auto path = score_tree->FromRank(rank);
    ScoreSds item = path.Terminal();
    if (range.max != cmaxstring) {
      int cmp = sdscmp((sds)item, range.max);
      if (cmp > 0 || (cmp == 0 && range.maxex))
        break;
    }
    ++deleted;
    score_tree->Delete(path);
    score_map->Erase((sds)item);
  }

  return deleted;
}

SortedMap::ScoredArray SortedMap::PopTopScores(unsigned count, bool reverse) {
  DCHECK_GT(count, 0u);
  DCHECK_EQ(score_map->UpperBoundSize(), score_tree->Size());
  size_t sz = score_map->UpperBoundSize();

  ScoredArray res;

  DCHECK_GT(sz, 0u);  // Empty sets are not allowed.

  if (sz == 0 || count == 0)
    return res;

  if (count > sz)
    count = sz;

  res.reserve(count);

  auto cb = [&](ScoreSds obj) {
    res.emplace_back(string{(sds)obj, sdslen((sds)obj)}, GetObjScore(obj));

    // We can not delete from score_tree because we are in the middle of the iteration.
    CHECK(score_map->Erase((sds)obj));
    return true;  // continue with the iteration.
  };

  unsigned rank = 0;
  unsigned step = 0;
  if (reverse) {
    score_tree->IterateReverse(0, count - 1, std::move(cb));
    rank = score_tree->Size() - 1;
    step = 1;
  } else {
    score_tree->Iterate(0, count - 1, std::move(cb));
  }

  // We already deleted elements from score_map, so what's left is to delete from the tree.
  if (score_map->Empty()) {
    // Corner case optimization.
    score_tree->Clear();
  } else {
    for (unsigned i = 0; i < res.size(); ++i) {
      auto path = score_tree->FromRank(rank);
      score_tree->Delete(path);
      rank -= step;
    }
  }

  return res;
}

size_t SortedMap::Count(const zrangespec& range) const {
  DCHECK_LE(range.min, range.max);

  if (score_tree->Size() == 0)
    return 0;

  // build min key.
  char buf[16];

  ScoreSds range_key = BuildScoredKey(range.min, buf);
  auto path = score_tree->GEQ(Query{range_key, false, range.minex});
  if (path.Empty())
    return 0;

  ScoreSds bound = path.Terminal();

  if (range.minex) {
    DCHECK_GT(GetObjScore(bound), range.min);
  } else {
    DCHECK_GE(GetObjScore(bound), range.min);
  }

  uint32_t min_rank = path.Rank();

  // Now build the max key.
  // If we need to exclude the maximum score, set the key'sstring part to empty string,
  // otherwise set it to infinity.
  range_key = BuildScoredKey(range.max, buf);
  path = score_tree->GEQ(Query{range_key, false, !range.maxex});
  if (path.Empty()) {
    return score_tree->Size() - min_rank;
  }

  bound = path.Terminal();
  uint32_t max_rank = path.Rank();
  if (range.maxex || GetObjScore(bound) > range.max) {
    if (max_rank <= min_rank)
      return 0;
    --max_rank;
  }

  // max_rank could be less than min_rank, for example, if the range is [a, a).
  return max_rank < min_rank ? 0 : max_rank - min_rank + 1;
}

size_t SortedMap::LexCount(const zlexrangespec& range) const {
  if (score_tree->Size() == 0)
    return 0;

  // Ranges that will always be zero - (+inf, anything) or (anything, -inf)
  if (range.min == cmaxstring || range.max == cminstring) {
    return 0;
  }

  uint32_t min_rank = 0;
  detail::BPTreePath<ScoreSds> path;

  if (range.min != cminstring) {
    path = score_tree->GEQ(Query{range.min, true});
    if (path.Empty())
      return 0;

    min_rank = path.Rank();
    if (range.minex && sdscmp((sds)path.Terminal(), range.min) == 0) {
      ++min_rank;
      if (min_rank >= score_tree->Size())
        return 0;
    }
  }

  uint32_t max_rank = score_tree->Size() - 1;
  if (range.max != cmaxstring) {
    path = score_tree->GEQ(Query{range.max, true});
    if (!path.Empty()) {
      max_rank = path.Rank();

      // fix the max rank, if needed.
      int cmp = sdscmp((sds)path.Terminal(), range.max);
      DCHECK_GE(cmp, 0);
      if (cmp > 0 || range.maxex) {
        if (max_rank <= min_rank)
          return 0;
        --max_rank;
      }
    }
  }

  return max_rank < min_rank ? 0 : max_rank - min_rank + 1;
}

bool SortedMap::Iterate(unsigned start_rank, unsigned len, bool reverse,
                        std::function<bool(sds, double)> cb) const {
  DCHECK_GT(len, 0u);
  unsigned end_rank = start_rank + len - 1;
  bool success;
  if (reverse) {
    success = score_tree->IterateReverse(
        start_rank, end_rank, [&](ScoreSds obj) { return cb((sds)obj, GetObjScore(obj)); });
  } else {
    success = score_tree->Iterate(start_rank, end_rank,
                                  [&](ScoreSds obj) { return cb((sds)obj, GetObjScore(obj)); });
  }

  return success;
}

uint64_t SortedMap::Scan(uint64_t cursor,
                         absl::FunctionRef<void(std::string_view, double)> cb) const {
  auto scan_cb = [&cb](const void* obj) {
    sds ele = (sds)obj;
    cb(string_view{ele, sdslen(ele)}, GetObjScore(obj));
  };

  return this->score_map->Scan(cursor, std::move(scan_cb));
}

// taken from zsetConvert
SortedMap* SortedMap::FromListPack(PMR_NS::memory_resource* res, const uint8_t* lp) {
  uint8_t* zl = (uint8_t*)lp;
  unsigned char *eptr, *sptr;
  unsigned char* vstr;
  unsigned int vlen;
  long long vlong;

  void* ptr = res->allocate(sizeof(SortedMap), alignof(SortedMap));
  SortedMap* zs = new (ptr) SortedMap{res};

  eptr = lpSeek(zl, 0);
  if (eptr != NULL) {
    sptr = lpNext(zl, eptr);
    CHECK(sptr != NULL);
  }

  while (eptr != NULL) {
    double score = ZzlGetScore(sptr);
    vstr = lpGetValue(eptr, &vlen, &vlong);
    if (vstr == NULL) {
      CHECK(zs->InsertNew(score, absl::StrCat(vlong)));
    } else {
      CHECK(zs->InsertNew(score, string_view{reinterpret_cast<const char*>(vstr), vlen}));
    }

    ZzlNext(zl, &eptr, &sptr);
  }

  return zs;
}

bool SortedMap::DefragIfNeeded(PageUsage* page_usage) {
  auto cb = [this](sds old_obj, sds new_obj) { score_tree->ForceUpdate(old_obj, new_obj); };
  bool reallocated = false;

  for (auto it = score_map->begin(); it != score_map->end(); ++it) {
    reallocated |= it.ReallocIfNeeded(page_usage, cb);
  }

  return reallocated;
}

std::optional<SortedMap::RankAndScore> SortedMap::GetRankAndScore(std::string_view ele,
                                                                  bool reverse) const {
  ScoreSds obj = score_map->FindObj(ele);
  if (obj == nullptr)
    return std::nullopt;

  optional rank = score_tree->GetRank(obj, reverse);
  DCHECK(rank);

  return SortedMap::RankAndScore{*rank, GetObjScore(obj)};
}
}  // namespace detail

sds cminstring = detail::kMinStrData + 1;
sds cmaxstring = detail::kMaxStrData + 1;

}  // namespace dfly
