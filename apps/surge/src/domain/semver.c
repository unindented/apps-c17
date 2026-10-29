#include "domain/semver.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core/ascii.h"
#include "core/str.h"

/**
 * @brief Parses a decimal number without a leading zero.
 *
 * @param text      Digits of the number.
 * @param value_out Receives the value. Must not be `NULL`.
 * @return `0` on success, or `-1` for an empty text, a non-digit, a leading zero, or an overflow.
 */
static int parse_number(struct Str text, ptrdiff_t* value_out) __attribute__((nonnull(2)));

/**
 * @brief Reports whether a pre-release identifier is valid: letters, digits and `-`, not empty, and
 *        without a leading zero when it is numeric.
 *
 * @param identifier The identifier.
 * @return `true` for a valid identifier.
 */
static bool is_pre_identifier(struct Str identifier);

/**
 * @brief Reports whether an identifier is all digits.
 *
 * @param identifier The identifier.
 * @return `true` for a numeric identifier.
 */
static bool is_numeric(struct Str identifier);

/**
 * @brief Orders two pre-release identifiers by precedence.
 *
 * @param a First identifier, valid.
 * @param b Second identifier, valid.
 * @return Negative, zero or positive as `a` precedes, equals, or follows `b`.
 */
static int compare_identifiers(struct Str a, struct Str b);

/**
 * @brief Orders two numbers.
 *
 * @param a First number.
 * @param b Second number.
 * @return `-1`, `0` or `1` as `a` is less than, equal to, or greater than `b`.
 */
static int compare_numbers(ptrdiff_t a, ptrdiff_t b);

int semver_parse(struct Str text, struct SemVer* version_out) {
  const struct StrCut pre = str_cut(text, '-');
  const struct StrCut major = str_cut(pre.head, '.');
  const struct StrCut minor = str_cut(major.tail, '.');
  if (!major.is_found || !minor.is_found) {
    return -1;
  }
  struct SemVer version = {.pre = pre.is_found ? pre.tail : (struct Str){0}};
  if (parse_number(major.head, &version.major) != 0 ||
      parse_number(minor.head, &version.minor) != 0 ||
      parse_number(minor.tail, &version.patch) != 0) {
    return -1;
  }
  if (pre.is_found) {
    struct StrCut cut = {.tail = version.pre, .is_found = true};
    while (cut.is_found) {
      cut = str_cut(cut.tail, '.');
      if (!is_pre_identifier(cut.head)) {
        return -1;
      }
    }
  }
  *version_out = version;
  return 0;
}

int semver_compare(const struct SemVer* a, const struct SemVer* b) {
  const ptrdiff_t numbers[][2] = {{a->major, b->major}, {a->minor, b->minor}, {a->patch, b->patch}};
  for (ptrdiff_t i = 0; i < 3; i++) {
    const int order = compare_numbers(numbers[i][0], numbers[i][1]);
    if (order != 0) {
      return order;
    }
  }
  // A release follows its pre-releases.
  if (a->pre.len == 0 || b->pre.len == 0) {
    return compare_numbers(a->pre.len == 0, b->pre.len == 0);
  }
  struct StrCut left = {.tail = a->pre, .is_found = true};
  struct StrCut right = {.tail = b->pre, .is_found = true};
  while (left.is_found && right.is_found) {
    left = str_cut(left.tail, '.');
    right = str_cut(right.tail, '.');
    const int order = compare_identifiers(left.head, right.head);
    if (order != 0) {
      return order;
    }
  }
  // Equal so far, the list with more identifiers follows.
  return compare_numbers(left.is_found, right.is_found);
}

static int parse_number(struct Str text, ptrdiff_t* value_out) {
  if (text.len == 0 || (text.len > 1 && text.data[0] == '0')) {
    return -1;
  }
  ptrdiff_t value = 0;
  for (ptrdiff_t i = 0; i < text.len; i++) {
    const unsigned char c = (unsigned char)text.data[i];
    if (!ascii_is_digit(c) || value > (PTRDIFF_MAX - (c - '0')) / 10) {
      return -1;
    }
    value = value * 10 + (c - '0');
  }
  *value_out = value;
  return 0;
}

static bool is_pre_identifier(struct Str identifier) {
  if (identifier.len == 0) {
    return false;
  }
  for (ptrdiff_t i = 0; i < identifier.len; i++) {
    const unsigned char c = (unsigned char)identifier.data[i];
    if (!ascii_is_alphanumeric(c) && c != '-') {
      return false;
    }
  }
  return !is_numeric(identifier) || identifier.len == 1 || identifier.data[0] != '0';
}

static bool is_numeric(struct Str identifier) {
  for (ptrdiff_t i = 0; i < identifier.len; i++) {
    if (!ascii_is_digit((unsigned char)identifier.data[i])) {
      return false;
    }
  }
  return true;
}

static int compare_identifiers(struct Str a, struct Str b) {
  const bool is_a_numeric = is_numeric(a);
  const bool is_b_numeric = is_numeric(b);
  if (is_a_numeric != is_b_numeric) {
    return is_a_numeric ? -1 : 1;
  }
  if (is_a_numeric) {
    // Without leading zeros, a longer number is a larger one.
    if (a.len != b.len) {
      return a.len < b.len ? -1 : 1;
    }
  }
  return str_compare(a, b);
}

static int compare_numbers(ptrdiff_t a, ptrdiff_t b) {
  return a < b ? -1 : a > b ? 1 : 0;
}
