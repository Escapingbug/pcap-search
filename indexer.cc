#include "common.hh"
using namespace std;

const char MAGIC_BAD[] = "BAD MEOW"; // first sizeof(off_t) bytes
const char MAGIC_GOOD[] = "GOODMEOW"; // first sizeof(off_t) bytes
const long LOGAB = CHAR_BIT, AB = 1L << LOGAB;

//const char *listen_path = "/tmp/search.sock";
long listen_port = 31223;
const pthread_t main_thread = pthread_self();
vector<const char *> data_dir;
string data_suffix = ".ap";
string index_suffix = ".fm";
long autocomplete_limit = 20;
long autocomplete_length = 20;
long search_limit = 20;
long fmindex_sample_rate = 32;
long indexer_limit = 0;
long rrr_sample_rate = 8;
double request_timeout = 1000;
long request_count = -1;
bool opt_force_rebuild = false;
bool opt_inotify = true;
bool opt_recursive = false;

///// common

ulong clog2(ulong x)
{
  return x > 1 ? sizeof(ulong)*CHAR_BIT-__builtin_clzl(x-1) : 0;
}

ulong select_in_u16(u16 x, ulong k)
{
  for (; k; k--)
    x &= x - 1;
  return __builtin_ctzl(x);
}

ulong select_in_ulong(ulong x, ulong k)
{
  ulong c;
#if ULONG_MAX == 0xffffffffffffffff
  c =  __builtin_popcountl(u16(x));
  if (c > k) return select_in_u16(x, k) + 0;
  x >>= 16;
  k -= c;
  c =  __builtin_popcountl(u16(x));
  if (c > k) return select_in_u16(x, k) + 16;
  x >>= 16;
  k -= c;
  c =  __builtin_popcountl(u16(x));
  if (c > k) return select_in_u16(x, k) + 32;
  x >>= 16;
  k -= c;
  return select_in_u16(x, k) + 48;
#elif ULONG_MAX == 0xffffffff
  c =  __builtin_popcountll(u16(x));
  if (c > k) return select_in_u16(x, k) + 0;
  x >>= 16;
  k -= c;
  return select_in_u16(x, k) + 16;
#else
# error "unsupported"
#endif
}

string escape(const string &str)
{
  const char ab[] = "0123456789abcdef";
  string ret;
  for (char c: str)
    if (isprint(c))
      ret += c;
    else {
      ret += "\\x";
      ret += ab[c>>4&15];
      ret += ab[c&15];
    }
  return ret;
}

string unescape(size_t n, const char *str)
{
  auto from_hex = [&](int c) {
    if ('0' <= c && c <= '9') return c-'0';
    if ('a' <= c && c <= 'f') return c-'a'+10;
    if ('A' <= c && c <= 'F') return c-'A'+10;
    return 0;
  };
  string ret;
  for (size_t i = 0; i < n; ) {
    if (str[i] == '\\') {
      if (i+4 <= n && str[i+1] == 'x') {
        ret.push_back(from_hex(str[i+2])*16+from_hex(str[i+3]));
        i += 4;
        continue;
      }
      if (i+1 <= n)
        switch (str[i+1]) {
        case 'a': ret += '\a'; i += 2; continue;
        case 'b': ret += '\b'; i += 2; continue;
        case 't': ret += '\t'; i += 2; continue;
        case 'n': ret += '\n'; i += 2; continue;
        case 'v': ret += '\v'; i += 2; continue;
        case 'f': ret += '\f'; i += 2; continue;
        case 'r': ret += '\r'; i += 2; continue;
        case '\\': ret += '\\'; i += 2; continue;
        }
      size_t j = i+1, v = 0;
      for (; j < n && j < i+4 && unsigned(str[j]-'0') < 8; j++)
        v = v*8+str[j]-'0';
      if (i+1 < j) {
        ret += char(v);
        i = j;
        continue;
      }
    }
    ret.push_back(str[i++]);
  }
  return ret;
}

///// vector

template<class T>
class SArray
{
  ulong n = 0;
  T *a = nullptr;
  bool is_created = false;
public:
  SArray() {}
  SArray(const SArray<T> &) = delete;

  void operator=(SArray<T> &&o) {
    n = o.n;
    a = o.a;
    is_created = o.is_created;
    o.n = 0;
    o.a = nullptr;
    o.is_created = false;
  }

  ~SArray() {
    if (is_created)
      delete[] a;
  }

  void init(ulong n) {
    assert(! a && ! is_created); // not loaded
    is_created = true;
    this->n = n;
    a = new T[n];
  }

  void init(ulong n, const T &x) {
    init(n);
    fill_n(a, n, x);
  }

  ulong size() const { return n; }
  T &operator[](ulong i) { return a[i]; }
  const T &operator[](ulong i) const { return a[i]; }
  T *begin() { return a; }
  T *end() { return a+n; }

  template<typename Archive>
  void serialize(Archive &ar) {
    ar.array(n, a);
  }

  template<typename Archive>
  void deserialize(Archive &ar) {
    ar & n;
    ar.align(alignof(T));
    a = (T*)ar.a;
    ar.skip(sizeof(T)*n);
  }
};

///// bitset

class BitSet
{
  ulong n;
  SArray<ulong> a;
public:
  static const long BITS = sizeof(long)*CHAR_BIT;
  BitSet() {}
  BitSet(ulong n) { init(n); }

  void init(ulong n) {
    this->n = n;
    a.init((n-1+BITS)/BITS, 0);
  }

  const SArray<ulong> &words() const { return a; }
  ulong size() const { return n; }

  void set(ulong x) { set(x, true); }

  void set(ulong x, bool b) {
    if (b)
      a[x/BITS] |= 1ul << x%BITS;
    else
      a[x/BITS] &= ~ (1ul << x%BITS);
  }

  bool operator[](ulong x) const {
    return a[x/BITS] & 1ul << x%BITS;
  }

  ulong get_bits(ulong x, ulong k) const {
    if (x % BITS + k <= BITS)
      return (a[x/BITS] >> x%BITS) & (1ul<<k)-1;
    return (a[x/BITS] >> x%BITS | a[x/BITS+1] << BITS-x%BITS) & (1ul<<k)-1;
  }

  ulong block(ulong k, ulong x) const { return get_bits(x*k, k); }

  void set_bits(ulong x, ulong k, ulong v) {
    if (! k) return;
    if (x % BITS + k <= BITS) {
      ulong i = x%BITS;
      a[x/BITS] = a[x/BITS] & ~ (((1ul<<k)-1) << i) | v << i;
    } else {
      ulong i = x%BITS, j = k-(BITS-i);
      a[x/BITS] = a[x/BITS] & ~ (-1ul<<i) | v << i;
      a[x/BITS+1] = a[x/64+1] & (-1ul<<j) | v >> 64-i;
    }
  }

  ulong popcount() const {
    ulong r = 0;
    REP(i, a.size())
      r += __builtin_popcountl(a[i]);
    return r;
  }

  template<typename Archive>
  void serialize(Archive &ar) {
    ar & n & a;
  }
};

///// suffix array

namespace KoAluru
{
  template<typename T>
  void bucket(T a[], int b[], int n, int k, bool end)
  {
    fill_n(b, k, 0);
    REP(i, n) b[a[i]]++;
    if (end)
      FOR(i, 1, k) b[i] += b[i-1];
    else {
      int s = 0;
      REP(i, k)
        s += b[i], b[i] = s-b[i];
    }
  }

  template<typename T>
  void plus_to_minus(T a[], int sa[], int b[], bool t[], int n, int k)
  {
    bucket(a, b, n, k, false);
    sa[b[a[n-1]]++] = n-1;
    REP(i, n-1) {
      int j = sa[i]-1;
      if (j >= 0 && ! t[j])
        sa[b[a[j]]++] = j;
    }
  }

  template<typename T>
  void minus_to_plus(T a[], int sa[], int b[], bool t[], int n, int k)
  {
    bucket(a, b, n, k, true);
    ROF(i, 0, n) {
      int j = sa[i]-1;
      if (j >= 0 && t[j])
        sa[--b[a[j]]] = j;
    }
  }

  template<typename T>
  void ka(T a[], int sa[], int b[], bool t[], int n, int k)
  {
    t[n-1] = false;
    ROF(i, 0, n-1)
      t[i] = a[i] < a[i+1] || a[i] == a[i+1] && t[i+1];
    bool minor = 2 * count(t, t+n, false) > n;

    bucket(a, b, n, k, minor);
    fill_n(sa, n, -1);
    if (minor) {
      REP(i, n)
        if (t[i])
          sa[--b[a[i]]] = i;
      plus_to_minus(a, sa, b, t, n, k);
      minus_to_plus(a, sa, b, t, n, k);
    } else {
      sa[b[a[n-1]]++] = n-1;
      REP(i, n-1)
        if (! t[i])
          sa[b[a[i]]++] = i;
      minus_to_plus(a, sa, b, t, n, k);
      plus_to_minus(a, sa, b, t, n, k);
    }

    int last = -1, name = 0, nn = count(t, t+n, minor);
    int *sa2, *pi;
    if (minor)
      sa2 = sa, pi = sa+n-nn;
    else
      sa2 = sa+n-nn, pi = sa;
    fill_n(b, n, -1);
    REP(i, n)
      if (sa[i] >= 0 && minor == t[sa[i]]) {
        bool diff = last == -1;
        int p = sa[i];
        if (! diff)
          REP(j, n) {
            if (last+j >= n || p+j >= n || a[last+j] != a[p+j] || t[last+j] != t[p+j]) {
              diff = true;
              break;
            } else if (j > 0 && (minor == t[last+j] || minor == t[p+j]))
              break;
          }
        if (diff) {
          name++;
          last = p;
        }
        b[p] = name-1;
      }
    nn = 0;
    REP(i, n)
      if (b[i] >= 0)
        pi[nn++] = b[i];

    if (name < nn)
      ka(pi, sa2, b, t, nn, name);
    else
      REP(i, nn)
        sa2[pi[i]] = i;

    ROF(i, 0, nn)
      t[i] = a[i] < a[i+1] || a[i] == a[i+1] && t[i+1];

    nn = 0;
    bucket(a, b, n, k, minor);
    if (minor) {
      REP(i, n)
        if (minor == t[i])
          pi[nn++] = i;
      REP(i, nn)
        sa[i] = pi[sa2[i]];
      ROF(i, 0, nn) {
        int j = sa[i];
        sa[i] = -1;
        sa[--b[a[j]]] = j;
      }
    } else {
      REP(i, n)
        if (minor == t[i])
          pi[nn++] = i;
      ROF(i, 0, nn)
        sa[n-nn+i] = pi[sa2[i]];
      REP(i, nn) {
        int j = sa[n-nn+i];
        sa[n-nn+i] = -1;
        sa[b[a[j]]++] = j;
      }
    }
    if (minor)
      plus_to_minus(a, sa, b, t, n, k);
    else
      minus_to_plus(a, sa, b, t, n, k);
  }

  template<typename T>
  void main(T a[], int sa[], int b[], int n, int k)
  {
    if (n > 0) {
      bool* t = new bool[n];
      ka(a, sa, b, t, n, k);
      delete[] t;
    }
  }
};

/// RRR

namespace RRRTable
{
  static const ulong SIZE = 20;
  static pthread_mutex_t rrr_mutex = PTHREAD_MUTEX_INITIALIZER;
  vector<vector<u32>> binom, offset_bits, combinations(SIZE), klass_offset(SIZE), offset_pos(SIZE);

  void init() {
    REP(i, SIZE) {
      combinations[i].resize(1ul<<i);
      klass_offset[i].resize(i+1);
      offset_pos[i].resize(1ul<<i);
      ulong pcomb = 0;
      REP(klass, i+1) {
        ulong j = 0, start = (1ul<<klass)-1, stop = start<<i-klass, x = start;
        klass_offset[i][klass] = pcomb;
        for(;;) {
          combinations[i][pcomb++] = x;
          offset_pos[i][x] = j++;
          if (x == stop) break;
          ulong y = x | x-1;
          x = y+1 | (~y&-~y)-1 >> __builtin_ctzl(x)+1;
        }
      }
      assert(pcomb == (1ul << i));
    }
  }

  void raise(long size) {
    pthread_mutex_lock(&rrr_mutex);
    FOR(i, binom.size(), size) {
      binom.emplace_back(i+1);
      binom[i][0] = binom[i][i] = 1;
      FOR(j, 1, i)
        binom[i][j] = binom[i-1][j-1]+binom[i-1][j];
      offset_bits.emplace_back(i+1);
      REP(j, i+1)
        offset_bits[i][j] = clog2(binom[i][j]);
    }
    pthread_mutex_unlock(&rrr_mutex);
  }
};

class RRR
{
  ulong n, block_len, sample_len, rank_sum, nblocks, nsamples, klass_bits, rsample_bits, osample_bits;
  BitSet klasses, offsets, rank_samples, offset_samples;

  ulong block2offset(ulong k, ulong x) const {
    if (block_len < RRRTable::SIZE)
      return RRRTable::offset_pos[block_len][x];
    ulong m = block_len-1, r = 0;
    for (; k; m--)
      if (x & 1ul << m) {
        if (k <= m)
          r += RRRTable::binom[m][k];
        k--;
      }
    return r;
  }

  ulong offset2block(ulong k, ulong off) const {
    if (block_len < RRRTable::SIZE)
      return RRRTable::combinations[block_len][RRRTable::klass_offset[block_len][k]+off];
    ulong m = block_len-1, r = 0;
    for (; k && k <= m; m--)
      if (RRRTable::binom[m][k] <= off) {
        off -= RRRTable::binom[m][k--];
        r |= 1ul << m;
      }
    if (k)
      r |= (1ul<<k) - 1;
    return r;
  }
public:
  void init(ulong n, ulong block_len, ulong sample_len, const BitSet &data) {
    this->n = n;
    this->block_len = block_len ? block_len : max(clog2(n), ulong(15));
    this->sample_len = sample_len ? sample_len : rrr_sample_rate;
    auto& binom = RRRTable::binom;
    auto& offset_bits = RRRTable::offset_bits;
    RRRTable::raise(this->block_len+1);
    build(data);
  }

  void build(const BitSet &data) {
    const auto& offset_bits = RRRTable::offset_bits[block_len];
    nblocks = (n-1+block_len)/block_len;
    rank_sum = 0;
    ulong offset_sum = 0, o = 0;
    REP(i, nblocks) {
      ulong val = data.get_bits(o, min(block_len, n-o)), klass = __builtin_popcountl(val);
      o += block_len;
      rank_sum += klass;
      offset_sum += offset_bits[klass];
    }
    nsamples = (nblocks-1+sample_len)/sample_len;
    klass_bits = clog2(block_len+1);
    rsample_bits = clog2(rank_sum);
    osample_bits = clog2(offset_sum);
    klasses.init(klass_bits*nblocks);
    offsets.init(offset_sum);
    rank_samples.init(rsample_bits*nsamples);
    offset_samples.init(osample_bits*nsamples);

    rank_sum = offset_sum = o = 0;
    REP(i, nblocks) {
      if (i % sample_len == 0) {
        rank_samples.set_bits(i/sample_len*rsample_bits, rsample_bits, rank_sum);
        offset_samples.set_bits(i/sample_len*osample_bits, osample_bits, offset_sum);
      }
      ulong val = data.get_bits(o, min(block_len, n-o)), klass = __builtin_popcountl(val);
      o += block_len;
      klasses.set_bits(klass_bits*i, klass_bits, klass);
      rank_sum += klass;
      offsets.set_bits(offset_sum, offset_bits[klass], block2offset(klass, val));
      offset_sum += offset_bits[klass];
    }
  }

  ulong zero_bits() const { return n-rank_sum; }

  ulong one_bits() const { return rank_sum; }

  bool operator[](ulong i) const {
    const auto& offset_bits = RRRTable::offset_bits[block_len];
    ulong b = i / block_len,
          bi = i % block_len,
          s = b / sample_len,
          j = s * sample_len,
          o = offset_samples.block(osample_bits, s);
    for (; j < b; j++)
      o += offset_bits[klasses.block(klass_bits, j)];
    ulong k = klasses.block(klass_bits, j);
    return offset2block(k, offsets.get_bits(o, offset_bits[k])) >> bi & 1;
  }

  ulong rank0(ulong i) const { return i-rank1(i); }

  ulong rank1(ulong i) const {
    const auto& offset_bits = RRRTable::offset_bits[block_len];
    ulong b = i / block_len,
          bi = i % block_len,
          s = b / sample_len,
          j = s * sample_len,
          r = rank_samples.block(rsample_bits, s),
          o = offset_samples.block(osample_bits, s),
          k;
    for (; j < b; j++) {
      k = klasses.block(klass_bits, j);
      r += k;
      o += offset_bits[k];
    }
    k = klasses.block(klass_bits, j);
    return r + __builtin_popcountl(offset2block(k, offsets.get_bits(o, offset_bits[k])) & (1ul<<bi)-1);
  }

  ulong select0(ulong kth) const {
    if (kth >= zero_bits()) return -1ul;
    const auto& offset_bits = RRRTable::offset_bits[block_len];
    ulong l = 0, h = nsamples;
    while (l < h) {
      ulong m = l+(h-l)/2, idx = m*sample_len*block_len;
      if (idx - rank_samples.block(rsample_bits, m) <= kth)
        l = m+1;
      else
        h = m;
    }

    ulong s = l-1,
          b = sample_len*s,
          r = block_len*b - rank_samples.block(rsample_bits, s),
          o = offset_samples.block(osample_bits, s),
          k;
    for (; ; b++) {
      k = klasses.block(klass_bits, b);
      if (r+block_len-k > kth) break;
      r += block_len-k;
      o += offset_bits[k];
    }

    o = offsets.get_bits(o, offset_bits[k]);
    return block_len*b + select_in_ulong(~ offset2block(k, o), kth-r);
  }

  ulong select1(ulong kth) const {
    if (kth >= rank_sum) return -1ul;
    const auto& offset_bits = RRRTable::offset_bits[block_len];
    ulong l = 0, h = nsamples;
    while (l < h) {
      ulong m = l+(h-l)/2;
      if (rank_samples.block(rsample_bits, m) <= kth)
        l = m+1;
      else
        h = m;
    }

    ulong s = l-1,
          b = sample_len*s,
          r = rank_samples.block(rsample_bits, s),
          o = offset_samples.block(osample_bits, s),
          k;
    for (; ; b++) {
      k = klasses.block(klass_bits, b);
      if (r+k > kth) break;
      r += k;
      o += offset_bits[k];
    }

    o = offsets.get_bits(o, offset_bits[k]);
    return block_len*b + select_in_ulong(offset2block(k, o), kth-r);
  }

  template<class Archive>
  void serialize(Archive &ar) {
    ar & n & block_len & sample_len & rank_sum & klasses & offsets & rank_samples & offset_samples;
  }

  template<class Archive>
  void deserialize(Archive &ar) {
    serialize(ar);
    nblocks = (n-1+block_len)/block_len;
    nsamples = (nblocks-1+sample_len)/sample_len;
    klass_bits = clog2(block_len+1);
    rsample_bits = clog2(rank_sum);
    osample_bits = clog2(offsets.size());
    RRRTable::raise(block_len+1);
  }
};

class EliasFanoBuilder
{
public:
  ulong n, bound, l, num = 0, pos = 0;
  BitSet lows, highs;

  EliasFanoBuilder(ulong n, ulong bound) : EliasFanoBuilder(n, bound, n && clog2(bound/n)) {}

  EliasFanoBuilder(ulong n, ulong bound, ulong l) : n(n), bound(bound), l(l), lows(l*n), highs((bound>>l)+n+1) {}

  void push(ulong x) {
    if (l) {
      lows.set_bits(pos, l, x & (1ul<<l)-1);
      pos += l;
    }
    highs.set((x>>l) + num++);
  }
};

class EliasFano
{
public:
  ulong n, bound, l;
  BitSet lows;
  RRR highs;
public:
  void init(EliasFanoBuilder &b) {
    n = b.n;
    bound = b.bound;
    l = b.l;
    lows = move(b.lows);
    highs.init((bound>>l)+n+1, 0, 0, b.highs);
  }

  ulong operator[](ulong idx) const {
    ulong ret = highs.select1(idx) - idx << l;
    if (l)
      ret |= lows.get_bits(l*idx, l);
    return ret;
  }

  ulong rank(ulong x) const {
    if (x > bound) return n;
    ulong hi = x >> l, lo = x & (1ul<<l)-1;
    ulong i = highs.select0(hi),
          r = i - hi; // number of elements in highs <= hi
    while (i && highs[i-1] && (l ? lows.get_bits((r-1)*l, l) : 0) >= lo)
      i--, r--;
    return r;
  }

  bool exist(ulong x) const {
    ulong r = rank(x);
    return r < n && operator[](r) == x;
  }

  template<typename Archive>
  void serialize(Archive &ar) {
    ar & n & bound & l & lows & highs;
  }
};

///// Wavelet Matrix

class WaveletMatrix
{
  ulong n;
  RRR rrr[LOGAB];
public:
  WaveletMatrix() {}
  ~WaveletMatrix() {}

  void init(ulong n, u8 *text, u8 *tmp) {
    this->n = n;
    BitSet bs(n);
    REP(d, LOGAB) {
      ulong bit = LOGAB-1-d;
      REP(i, n)
        bs.set(i, text[i] >> bit & 1);
      rrr[d].init(n, 0, 0, bs);
      if (d < LOGAB-1) {
        ulong j = 0;
        REP(i, n)
          if (! (text[i] >> bit & 1))
            tmp[j++] = text[i];
        REP(i, n)
          if (text[i] >> bit & 1)
            tmp[j++] = text[i];
        swap(text, tmp);
      }
    }
  }

  ulong operator[](ulong i) const { return at(i); }
  ulong at(ulong i) const {
    return at(0, 0, AB, i);
  }
  ulong at(ulong d, ulong l, ulong h, ulong i) const {
    if (h-l == 1) return l;
    ulong m = l+h >> 1, z = rrr[d].zero_bits();
    return ! rrr[d][i]
      ? at(d+1, l, m, rrr[d].rank0(i))
      : at(d+1, m, h, z+rrr[d].rank1(i));
  }

  // number of occurrences of symbol `x` in [0,i)
  ulong rank(ulong x, ulong i) const {
    return rank(0, 0, AB, x, i, 0);
  }
  ulong rank(ulong d, ulong l, ulong h, ulong x, ulong i, ulong p) const {
    if (h-l == 1) return i-p;
    ulong m = l+h >> 1, z = rrr[d].zero_bits();
    return x < m
      ? rank(d+1, l, m, x, rrr[d].rank0(i), rrr[d].rank0(p))
      : rank(d+1, m, h, x, z+rrr[d].rank1(i), z+rrr[d].rank1(p));
  }
  // position of `k`-th occurrence of symbol `x`
  ulong select(ulong x, ulong k) const {
    return select(0, 0, AB, x, k, 0);
  }
  ulong select(ulong d, ulong l, ulong h, ulong x, ulong k, ulong p) const {
    if (l == h-1) return p+k;
    ulong m = l+h >> 1, z = rrr[d].zero_bits();
    return x < m
      ? rrr[d].select0(select(d+1, l, m, x, k, rrr[d].rank0(p)))
      : rrr[d].select1(select(d+1, m, h, x, k, z+rrr[d].rank1(p)) - z);
  }

  template<typename Archive>
  void serialize(Archive &ar) {
    ar & n;
    REP(i, LOGAB)
      ar & rrr[i];
  }
};

///// FM-index

class FMIndex
{
  ulong n_, samplerate_, initial_;
  ulong cnt_lt_[AB+1];
  EliasFano sampled_ef_;
  SArray<u32> ssa_;
  WaveletMatrix bwt_wm_;
public:
  void init(ulong n, const u8 *text, ulong samplerate) {
    samplerate_ = samplerate;
    n_ = n;

    ulong cnt = 0;
    fill_n(cnt_lt_, AB, 0);
    REP(i, n)
      cnt_lt_[text[i]]++;
    REP(i, AB) {
      ulong t = cnt_lt_[i];
      cnt_lt_[i] = cnt;
      cnt += t;
    }
    cnt_lt_[AB] = cnt;

    int *sa = new int[n];
    int *tmp = new int[max(n, ulong(AB))];
    ulong sampled_n = (n-1+samplerate)/samplerate;
    EliasFanoBuilder efb(sampled_n, n ? n-1 : 0);
    ssa_.init(sampled_n);

    ulong nn = 0;
    KoAluru::main(text, sa, tmp, n, AB);
    REP(i, n)
      if (sa[i] % samplerate == 0) {
        ssa_[nn++] = sa[i];
        efb.push(i);
      }
    sampled_ef_.init(efb);

    // 'initial' is the position of '$' in BWT of text+'$'
    // BWT of text (sentinel character is implicit)
    // sizeof(int) >= 2*sizeof(u8)
    u8 *bwt = (u8 *)tmp, *bwt_t = (u8 *)tmp+n;
    initial_ = -1;
    if (n) {
      bwt[0] = text[n-1];
      REP(i, n)
        if (! sa[i])
          initial_ = i+1;
        else
          bwt[i + (initial_ == -1)] = text[sa[i]-1];
      bwt_wm_.init(n, bwt, bwt_t);
    }
    delete[] tmp;
    delete[] sa;
  }
  // backward search: count occurrences in rotated string
  pair<ulong, ulong> get_range(ulong m, const u8 *pattern) const {
    if (! m)
      return {0, n_};
    u8 c = pattern[m-1];
    ulong i = m-1, l = cnt_lt_[c], h = cnt_lt_[c+1];
    // [l, h) denotes rows [l+1,h+1) of BWT matrix of text+'$'
    // row 'i' of the first column of BWT matrix is mapped to row i+(i<initial_) of the last column
    while (l < h && i) {
      c = pattern[--i];
      l = cnt_lt_[c] + bwt_wm_.rank(c, l + (l < initial_));
      h = cnt_lt_[c] + bwt_wm_.rank(c, h + (h < initial_));
    }
    return {l, h};
  }
  // m > 0
  ulong count(ulong m, const u8 *pattern) const {
    if (! m) return n_;
    auto x = get_range(m, pattern);
    return x.second-x.first;
  }

  ulong calc_sa(ulong rank) const {
    ulong d = 0, i = rank;
    while (! sampled_ef_.exist(i)) {
      ulong c = bwt_wm_[i + (i < initial_)];
      i = cnt_lt_[c] + bwt_wm_.rank(c, i + (i < initial_));
      d++;
    }
    return ssa_[sampled_ef_.rank(i)] + d;
  }

  ulong locate(ulong m, const u8 *pattern, bool autocomplete, ulong limit, ulong &skip, vector<ulong> &res) const {
    ulong l, h, total;
    tie(l, h) = get_range(m, pattern);
    total = h-l;
    ulong delta = min(h-l, skip);
    l += delta;
    skip -= delta;
    ulong step = autocomplete ? max((h-l)/limit, ulong(1)) : 1;
    for (; l < h && res.size() < limit; l += step)
      res.push_back(calc_sa(l));
    return total;
  }

  template<typename Archive>
  void serialize(Archive &ar) {
    ar & n_ & samplerate_ & initial_;
    REP(i, LEN_OF(cnt_lt_))
      ar & cnt_lt_[i];
    ar & sampled_ef_;
    ar & ssa_;
    ar & bwt_wm_;
  }
};

// serialization
//
// http://stackoverflow.com/questions/257288/is-it-possible-to-write-a-c-template-to-check-for-a-functions-existence
struct Serializer
{
  FILE *fh;

  Serializer(FILE *fh) : fh(fh) {}

  template<class T>
  auto serialize_imp(T &x, int) -> decltype(x.serialize(*this), void()) {
    x.serialize(*this);
  }

  template<class T>
  void serialize_imp(T &x, long) {
    fwrite(&x, sizeof x, 1, fh);
  }

  template<class T>
  Serializer& operator&(T &x) {
    serialize_imp(x, 0);
    return *this;
  }

  Serializer& operator&(ulong &x) {
    // for 64-bits: serialized as int to save space
    fwrite(&x, sizeof(int), 1, fh);
    return *this;
  }

  template<class S, class T>
  void array(S n, T *a) {
    operator&(n);
    align(alignof(T));
    REP(i, n)
      operator&(a[i]);
  }

  template<class S>
  void array(S n, ulong *a) {
    operator&(n);
    align(alignof(ulong));
    REP(i, n)
      fwrite(&a[i], sizeof(ulong), 1, fh);
  }

  void align(size_t n) {
    off_t o = ftello(fh);
    if (o == -1)
      err_exit(EX_IOERR, "ftello");
    if (o%n && fseeko(fh, o+n-o%n, SEEK_SET))
      err_exit(EX_IOERR, "fseeko");
  }
};

struct Deserializer
{
  void *a;

  Deserializer(void *a) : a(a) {}

  template<class T>
  Deserializer &operator&(T &x) {
    deserialize_imp0(x, 0);
    return *this;
  }

  Deserializer& operator&(ulong &x) {
    x = 0;
    memcpy(&x, a, sizeof(int));
    a = (int *)a + 1;
    return *this;
  }

  // has .deserialize
  template<class T>
  auto deserialize_imp0(T &x, int) -> decltype(x.deserialize(*this), void()) {
    x.deserialize(*this);
  }
  template<class T>
  void deserialize_imp0(T &x, long) {
    deserialize_imp1(x, 0);
  }

  // has .serialize
  template<class T>
  auto deserialize_imp1(T &x, int) -> decltype(x.serialize(*this), void()) {
    x.serialize(*this);
  }

  // fallback
  template<class T>
  void deserialize_imp1(T &x, long) {
    memcpy(&x, a, sizeof(T));
    a = (T *)a + 1;
  }

  void align(size_t n) {
    auto o = (uintptr_t)a % n;
    if (o)
      a = (void*)((uintptr_t)a+o);
  }

  void skip(size_t n) {
    a = (void*)((uintptr_t)a+n);
  }
};

void print_help(FILE *fh)
{
  fprintf(fh, "Usage: %s [OPTIONS] dir\n", program_invocation_short_name);
  fputs(
        "\n"
        "Options:\n"
        "  --autocomplete-length %ld\n"
        "  --autocomplete-limit %ld  max number of autocomplete items\n"
        "  -c, --request-count %ld   max number of requests (default: -1)\n"
        "  -f, --force-rebuild       ignore exsistent indices\n"
        "  --fmindex-sample-rate %lf sample rate of suffix array (for rank -> pos) used in FM index\n"
        "  -P, --indexer-limit %ld   max number of concurrent indexing tasks\n"
        "  -l, --search-limit %ld    max number of results\n"
        "  --rrr-sample-rate %ld     R blocks are grouped to a superblock\n"
        "  -o, --oneshot             run only once (no inotify)\n"
        "  -p, --path %s             path of listening Unix domain socket\n"
        "  -r, --recursive           recursive\n"
        "  -s, --data-suffix %s      data file suffix. (default: .ap)\n"
        "  -S, --index-suffix %s     index file suffix. (default: .fm)\n"
        "  -t, --request-timeout %lf clients idle for more than T seconds will be dropped (default: 1)\n"
        "  -h, --help                display this help and exit\n"
        "\n"
        "Examples:\n"
        "  zsh0: ./indexer -o /tmp/ray # build index oneshot and run server\n"
        "  zsh0: ./indexer /tmp/ray # build index and watch changes within /tmp/ray, creating indices upon CLOSE_WRITE after CREATE/MODIFY, and MOVED_TO, removing indices upon DELETE and MOVED_FROM\n"
        "  zsh1: print -rn -- $'\\0\\0\\0haystack' | socat -t 60 - /tmp/search.sock # autocomplete\n"
        "  zsh1: print -rn -- $'3\\0\\0\\0haystack' | socat -t 60 - /tmp/search.sock # search, skip first 3 matches\n"
        "  zsh1: print -rn -- $'5\\0a\\0b\\0ha\\0stack\\0\\\\0\\\\1' | socat -t 60 - /tmp/search.sock # search filenames F satisfying (\"a\" <= F <= \"b\"), skip first 5, pattern is \"stack\\0\\0\\1\". \\-escape is allowed\n"
        , fh);
  exit(fh == stdout ? 0 : EX_USAGE);
}

struct Entry
{
  FILE* index_fh;
  int data_fd, index_fd;
  off_t data_size, index_size;
  void *data_mmap, *index_mmap;
  FMIndex *fm;
  ~Entry() {
    delete fm;
    munmap(data_mmap, data_size);
    munmap(index_mmap, index_size);
    close(data_fd);
    if (index_fh)
      fclose(index_fh);
    else
      close(index_fd);
  }
};

string data_to_index(const string& path)
{
  return path+index_suffix;
}

bool is_data(const string &path)
{
  return path.size() >= data_suffix.size() && path.substr(path.size()-data_suffix.size()) == data_suffix;
}

string to_path(string path, string name)
{
  if (! (path.size() && path.back() == '/'))
    path += '/';
  if (name.size() && name[0] == '/')
    name = name.substr(1);
  path += name;
  return path;
}

template<> vector<RefCountTreap<string, shared_ptr<Entry>>::Node*> RefCountTreap<string, shared_ptr<Entry>>::roots{};

namespace Server
{
  int inotify_fd = -1, pending = 0, pending_indexers = 0;
  map<int, string> wd2dir;
  map<string, int> dir2wd;
  set<string> modified;

  pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
  pthread_cond_t manager_cond = PTHREAD_COND_INITIALIZER,
                 pending_empty = PTHREAD_COND_INITIALIZER;
  bool manager_quit = false;
  vector<string> indexer_tasks;
  RefCountTreap<string, shared_ptr<Entry>> loaded;

  void detached_thread(void* (*start_routine)(void*), void* data) {
    pending++;
    pthread_t tid;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    if (pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED))
      err_exit(EX_OSERR, "pthread_attr_setdetachstate");
    if (pthread_create(&tid, &attr, start_routine, data))
      err_exit(EX_OSERR, "pthread_create");
    pthread_attr_destroy(&attr);
  }

  int inotify_add_dir(const string& dir) {
    int wd = inotify_add_watch(inotify_fd, dir.c_str(), IN_CLOSE_WRITE | IN_CREATE | IN_DELETE | IN_IGNORED | IN_MODIFY | IN_MOVE | IN_MOVE_SELF);
    if (wd < 0) {
      err_msg("failed to inotify_add_watch %s", dir.c_str());
      return wd;
    }
    wd2dir[wd] = dir;
    dir2wd[dir] = wd;
    log_action("inotify_add_watch %s", dir.c_str());
    return wd;
  }

  void add_data(const string& data_path) {
    indexer_tasks.push_back(data_path);
    pthread_cond_signal(&manager_cond);
  }

  void rm_data(const string& data_path) {
    string index_path = data_to_index(data_path);
    if (! unlink(index_path.c_str()))
      log_action("unlinked %s", index_path.c_str());
    else if (errno != ENOENT)
      err_msg("failed to unlink %s", index_path.c_str());
    if (loaded.find(data_path)) {
      loaded.erase(data_path);
      log_action("unloaded index of %s", data_path.c_str());
    }
  }

  void* indexer(void* data_path_) {
    string* data_path = (string*)data_path_;
    string index_path = data_to_index(*data_path);
    int data_fd = -1, index_fd = -1;
    off_t data_size, index_size;
    void *data_mmap = MAP_FAILED, *index_mmap = MAP_FAILED;
    bool rebuild = true;
    FILE* fh = NULL;
    errno = 0;
    if ((data_fd = open(data_path->c_str(), O_RDONLY)) < 0)
      goto quit;
    if ((data_size = lseek(data_fd, 0, SEEK_END)) < 0)
      goto quit;
    if (data_size > 0 && (data_mmap = mmap(NULL, data_size, PROT_READ, MAP_SHARED, data_fd, 0)) == MAP_FAILED)
      goto quit;
    if ((index_fd = open(index_path.c_str(), O_RDWR | O_CREAT, 0666)) < 0)
      goto quit;
    {
      off_t buf[2];
      int nread;
      if ((nread = read(index_fd, buf, sizeof buf)) < 0)
        goto quit;
      else if (nread == 0)
       ;
      else if (nread < sizeof(off_t) || memcmp(buf, MAGIC_GOOD, sizeof(off_t)))
        log_status("index file %s: bad magic, rebuilding", index_path.c_str());
      else if (nread < 2*sizeof(off_t) || buf[1] != data_size)
        log_status("index file %s: mismatching length of data file, rebuilding", index_path.c_str());
      else if ((index_size = lseek(index_fd, 0, SEEK_END)) < 2*sizeof(off_t))
        ;
      else if (! opt_force_rebuild)
        goto load;
    }
    // rebuild
    if (loaded.find(*data_path)) {
      loaded.erase(*data_path);
      log_action("rebuilding index of '%s", data_path->c_str());
    }
    {
      StopWatch sw;
      if (! (fh = fdopen(index_fd, "w")))
        goto quit;
      if (fseeko(fh, 0, SEEK_SET) < 0)
        err_exit(EX_IOERR, "fseeko");
      if (fwrite(MAGIC_BAD, sizeof(off_t), 1, fh) != 1)
        err_exit(EX_IOERR, "fwrite");
      if (fwrite(MAGIC_BAD, sizeof(off_t), 1, fh) != 1) // length of origin
        err_exit(EX_IOERR, "fwrite");
      Serializer ar(fh);
      FMIndex fm;
      fm.init(data_size, (const u8 *)data_mmap, fmindex_sample_rate);
      ar & fm;
      index_size = ftello(fh);
      if (ftruncate(index_fd, index_size) < 0)
        err_exit(EX_IOERR, "ftruncate");
      if (fseeko(fh, 0, SEEK_SET) < 0)
        err_exit(EX_IOERR, "fseeko");
      if (fwrite(MAGIC_GOOD, sizeof(off_t), 1, fh) != 1)
        err_exit(EX_IOERR, "fwrite");
      if (fwrite(&data_size, sizeof(off_t), 1, fh) != 1)
        err_exit(EX_IOERR, "fwrite");
      if (fflush(fh) == EOF)
        err_exit(EX_IOERR, "fflush");
      log_action("created index of %s. data: %ld, index: %ld, used %.3lf s", data_path->c_str(), data_size, index_size, sw.elapsed());
    }
load:
    {
      if ((index_mmap = mmap(NULL, index_size, PROT_READ, MAP_SHARED, index_fd, 0)) == MAP_FAILED)
        goto quit;
      Deserializer ar((u8*)index_mmap+2*sizeof(off_t));
      auto entry = make_shared<Entry>();
      entry->data_fd = data_fd;
      entry->index_fd = index_fd;
      entry->index_fh = fh;
      entry->data_size = data_size;
      entry->index_size = index_size;
      entry->data_mmap = data_mmap;
      entry->index_mmap = index_mmap;
      entry->fm = new FMIndex;
      ar & *entry->fm;
      pthread_mutex_lock(&mutex);
      loaded.insert(*data_path, entry);
      pthread_cond_signal(&manager_cond);
      pthread_mutex_unlock(&mutex);
      log_action("loaded index of %s", data_path->c_str());
    }
    goto success;
quit:
    if (fh)
      fclose(fh);
    else if (index_fd >= 0)
      close(index_fd);
    if (data_mmap != MAP_FAILED)
      munmap(data_mmap, data_size);
    if (data_fd >= 0)
      close(data_fd);
success:
    delete data_path;
    if (errno)
      err_msg("failed to index %s", data_path->c_str());
    pthread_mutex_lock(&mutex);
    pending--;
    pending_indexers--;
    pthread_cond_signal(&manager_cond);
    pthread_mutex_unlock(&mutex);
    return NULL;
  }

  void walk(long depth, long dir_fd, string path, const char* file) {
    int fd = -1;
    struct stat statbuf;
    if (stat(path.c_str(), &statbuf) < 0)
      err_msg_g("stat");
    if (S_ISREG(statbuf.st_mode)) {
      if (is_data(path)) add_data(path);
    } else if (S_ISDIR(statbuf.st_mode)) {
      if (! opt_recursive && depth > 0) goto quit;
      if (inotify_fd >= 0)
        inotify_add_dir(path);
      fd = openat(dir_fd, file, O_RDONLY);
      if (fd < 0)
        err_msg_g("failed to open %s", path.c_str());
      DIR* dirp = fdopendir(fd);
      if (! dirp)
        err_msg_g("opendir");
      struct dirent dirent, *dirt;
      while (! readdir_r(dirp, &dirent, &dirt) && dirt)
        if (strcmp(dirent.d_name, ".") && strcmp(dirent.d_name, ".."))
          walk(depth+1, fd, to_path(path, dirent.d_name), dirent.d_name);
      closedir(dirp);
      fd = -1;
    }

quit:;
     if (fd >= 0)
       close(fd);
  }

  void process_inotify() {
    char buf[sizeof(inotify_event)+NAME_MAX+1];
    int nread;
    if ((nread = read(inotify_fd, buf, sizeof buf)) <= 0)
      err_exit(EX_OSERR, "failed to read inotify fd");
    errno = 0;
    for (auto *ev = (inotify_event *)buf; (char *)ev < (char *)buf+nread;
         ev = (inotify_event *)((char *)ev + sizeof(inotify_event) + ev->len))
      if (ev->len > 0 || ev->mask & (IN_IGNORED | IN_MOVE_SELF)) {
        const char* dir = wd2dir[ev->wd].c_str();
        bool data = is_data(ev->name);
        string path = to_path(dir, ev->name);
        if (ev->mask & (IN_CREATE | IN_MOVED_TO)) {
          if (ev->mask & IN_CREATE)
            log_event("CREATE %s", path.c_str());
          else
            log_event("MOVED_TO %s", path.c_str());

          if (ev->mask & IN_ISDIR)
            opt_recursive && inotify_add_dir(path.c_str());
          else if (data) {
            struct stat statbuf;
            if (lstat(path.c_str(), &statbuf) < 0) continue;
            if (ev->mask & IN_MOVED_TO || S_ISLNK(statbuf.st_mode)) {
              modified.erase(path);
              add_data(path);
            } else
              modified.insert(path);
          }
        } else if (ev->mask & (IN_DELETE | IN_MOVED_FROM)) {
          if (ev->mask & IN_DELETE)
            log_event("DELETE %s", path.c_str());
          else
            log_event("MOVED_FROM %s", path.c_str());
          if (! (ev->mask & IN_ISDIR)) {
            modified.erase(path);
            if (data) rm_data(path);
          }
        } else if (ev->mask & IN_IGNORED) {
          log_event("IGNORED %s", dir);
          if (wd2dir.count(ev->wd)) {
            dir2wd.erase(wd2dir[ev->wd]);
            wd2dir.erase(ev->wd);
          }
        } else if (ev->mask & IN_MODIFY) {
          if (data) modified.insert(path);
        } else if (ev->mask & IN_MOVE_SELF)
          err_exit(EX_OSFILE, "%s has been moved", wd2dir[ev->wd].c_str());
        else if (ev->mask & IN_CLOSE_WRITE) {
          if (modified.count(path)) {
            log_event("CLOSE_WRITE after MODIFY %s", path.c_str());
            modified.erase(path);
            if (data) add_data(path);
          }
        }
      }
  }

  void* request_worker(void* connfd_) {
    int connfd = intptr_t(connfd_);
    char buf[BUF_SIZE] = {};
    const char *p, *file_begin = buf, *file_end = nullptr;
    int nread = 0;
    timespec timeout;
    {
      double tmp;
      timeout.tv_sec = modf(request_timeout, &tmp);
      timeout.tv_nsec = tmp;
    }

    struct pollfd fds;
    fds.fd = connfd;
    fds.events = POLLIN;

    
    for(;;) {
	    /*
      int ready = ppoll(&fds, 1, &timeout, NULL);
      if (ready < 0) {
        if (errno == EINTR) continue;
        goto quit;
      }
      if (! ready) goto quit; // timeout
      */ssize_t t = read(connfd, buf+nread, sizeof buf-nread);
      if (t < 0) goto quit;
      if (! t) break;
      nread += t;
    }

    for (p = buf; p < buf+nread && *p; p++);
    if (++p >= buf+nread) goto quit;
    file_begin = p;
    for (; p < buf+nread && *p; p++);
    if (++p >= buf+nread) goto quit;
    file_end = p;
    for (; p < buf+nread && *p; p++);
    ulong len;
    if (p+1 < buf+nread) {
      p++;
      len = buf+nread-p;
    } else
      len = 0;

    {
      pthread_mutex_lock(&mutex);
      auto* root = loaded.root;
      if (root) root->refcnt++;
      pthread_mutex_unlock(&mutex);

      vector<ulong> res;
      ulong total = 0;
      string pattern = unescape(len, p), low, high("\xff"); // assume low <= filepath <= high
      auto range = loaded.range_backward(root, low, high, *file_begin ? string(file_begin) : low, *file_end ? string(file_end) : high);
      // autocomplete
      if (! buf[0]) {
        typedef tuple<string, ulong, string> cand_type;
        ulong skip = 0;
        vector<cand_type> candidates;
        for (auto& it: range) {
          auto entry = it.val;
          auto old_size = res.size();
          entry->fm->locate(pattern.size(), (const u8*)pattern.c_str(), true, autocomplete_limit, skip, res);
          FOR(i, old_size, res.size())
            candidates.emplace_back(it.key, res[i], string((char*)entry->data_mmap+res[i], (char*)entry->data_mmap+min(ulong(entry->data_size), res[i]+len+autocomplete_length)));
          if (res.size() >= autocomplete_limit) break;
        }
        sort(candidates.begin(), candidates.end(), [](const cand_type& x, const cand_type& y) { return get<2>(x) < get<2>(y); });
        candidates.erase(unique(candidates.begin(), candidates.end(), [](const cand_type &x, const cand_type &y) { return get<2>(x) == get<2>(y); }), candidates.end());
        for (auto& cand: candidates)
          if (dprintf(connfd, "%s\t%lu\t%s\n", get<0>(cand).c_str(), get<1>(cand), escape(get<2>(cand)).c_str()) < 0)
            goto quit;
      } else {
        char *end;
        errno = 0;
        ulong skip = strtoul(buf, &end, 0);
        if (! *end && ! errno) {
          for (auto& it: range) {
            auto entry = it.val;
            auto old_size = res.size();
            total += entry->fm->locate(pattern.size(), (const u8*)pattern.c_str(), false, search_limit, skip, res);
            FOR(i, old_size, res.size())
              if (dprintf(connfd, "%s\t%lu\t%lu\n", it.key.c_str(), res[i], len) < 0)
                goto quit;
            if (res.size() >= search_limit) break;
          }
          dprintf(connfd, "%lu\n", total);
        }
      }

      pthread_mutex_lock(&mutex);
      if (root) root->unref();
      pthread_mutex_unlock(&mutex);
    }
quit:
    close(connfd);
    pthread_mutex_lock(&mutex);
    if (! --pending)
      pthread_cond_signal(&pending_empty);
    pthread_mutex_unlock(&mutex);
    return NULL;
  }

  void* manager(void*) {
    for(;;) {
      pthread_mutex_lock(&mutex);
      while (! manager_quit && loaded.roots.empty() && (indexer_tasks.empty() || pending_indexers >= indexer_limit))
        pthread_cond_wait(&manager_cond, &mutex);
      while (indexer_tasks.size() && pending_indexers < indexer_limit) {
        pending_indexers++;
        detached_thread(indexer, new string(indexer_tasks.back()));
        indexer_tasks.pop_back();
      }
      while (loaded.roots.size()) {
        if (loaded.roots.back())
          loaded.roots.back()->unref();
        loaded.roots.pop_back();
      }
      pthread_mutex_unlock(&mutex);
      if (manager_quit) break;
    }
    pthread_mutex_lock(&mutex);
    if (! --pending)
      pthread_cond_signal(&pending_empty);
    pthread_mutex_unlock(&mutex);
    return NULL;
  }

  void run() {
    signal(SIGPIPE, SIG_IGN); // SIGPIPE while writing to clients

    /*
    int sockfd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sockfd < 0)
      err_exit(EX_OSERR, "socket");
    struct sockaddr_un addr = {};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, listen_path, sizeof(addr.sun_path)-1);
    if (! unlink(listen_path))
      log_action("removed old socket %s", listen_path);
    if (bind(sockfd, (struct sockaddr *)&addr, sizeof addr) < 0)
      err_exit(EX_OSERR, "bind");
    if (listen(sockfd, 1) < 0)
      err_exit(EX_OSERR, "listen");
    log_status("listening on %s", listen_path);
    */

    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
      err_exit(EX_OSERR, "socket");
    }

    int enable = 1;
    if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable)) < 0) {
      err_exit(EX_OSERR, "setsockopt");
    }
    struct sockaddr_in address;
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(listen_port);

    if (bind(sockfd, (struct sockaddr *) &address,
          sizeof(address)) < 0) {
      err_exit(EX_OSERR, "bind");
    }

    if (listen(sockfd, 1) < 0) {
      err_exit(EX_OSERR, "listen");
    }
    log_status("listening on port %d", listen_port);

    // load existing
    if (opt_inotify)
      if ((inotify_fd = inotify_init()) < 0)
        err_exit(EX_OSERR, "inotify_init");
    for (auto dir: data_dir)
      walk(0, AT_FDCWD, dir, dir);
    if (opt_inotify)
      log_status("start inotify");
    pthread_mutex_lock(&mutex);
    detached_thread(manager, nullptr);
    pthread_mutex_unlock(&mutex);

    while (request_count) {
      struct pollfd fds[2];
      fds[0].fd = sockfd;
      fds[0].events = POLLIN;
      int nfds = 1;
      if (inotify_fd >= 0) {
        fds[1].fd = inotify_fd;
        fds[1].events = POLLIN;
        nfds = 2;
      }
      int ready = poll(fds, nfds, -1);
      if (ready < 0) {
        if (errno == EINTR) continue;
        err_exit(EX_OSERR, "poll");
      }
      if (fds[0].revents & POLLIN) { // socket
        int connfd = accept(sockfd, NULL, NULL);
        if (connfd < 0) err_exit(EX_OSERR, "accept");
        pthread_mutex_lock(&mutex);
        detached_thread(request_worker, (void*)(intptr_t)connfd);
        pthread_mutex_unlock(&mutex);
        if (request_count > 0) request_count--;
      }
      if (1 < nfds && fds[1].revents & POLLIN) // inotifyfd
        process_inotify();
    }
    if (inotify_fd >= 0)
      close(inotify_fd);
    close(sockfd);
    // destructors should be called after all readers & writers of RefCountTreap have finished
    pthread_mutex_lock(&mutex);
    manager_quit = true;
    pthread_cond_signal(&manager_cond);
    while (pending > 0)
      pthread_cond_wait(&pending_empty, &mutex);
    pthread_mutex_unlock(&mutex);
    for (auto x: loaded.roots)
      if (x)
        x->unref();
    loaded.clear();
  }
}

int main(int argc, char *argv[])
{
  int opt;
  static struct option long_options[] = {
    {"autocomplete-length", required_argument, 0,   2},
    {"autocomplete-limit",  required_argument, 0,   3},
    {"data-suffix",         required_argument, 0,   's'},
    {"fmindex-sample-rate", required_argument, 0,   4},
    {"force-rebuild",       no_argument,       0,   'f'},
    {"help",                no_argument,       0,   'h'},
    {"indexer-limit",       required_argument, 0,   'P'},
    {"index-suffix",        required_argument, 0,   'S'},
    {"oneshot",             no_argument,       0,   'o'},
    {"port",                required_argument, 0,   'p'},
    {"recursive",           no_argument,       0,   'r'},
    {"request-count",       required_argument, 0,   'c'},
    {"request-timeout",     required_argument, 0,   't'},
    {"rrr-sample-rate",     required_argument, 0,   5},
    {0,                     0,                 0,   0},
  };

  while ((opt = getopt_long(argc, argv, "-c:fhl:op:P:rs:S:t:", long_options, NULL)) != -1) {
    switch (opt) {
    case 1: {
      struct stat statbuf;
      if (stat(optarg, &statbuf) < 0)
        err_exit(EX_OSFILE, "stat");
      if (! S_ISDIR(statbuf.st_mode))
        err_exit(EX_USAGE, "%s is not a directory", optarg);
      data_dir.push_back(optarg);
      break;
    }
    case 2:
      autocomplete_length = get_long(optarg);
      break;
    case 3:
      autocomplete_limit = get_long(optarg);
      break;
    case 4:
      fmindex_sample_rate = get_double(optarg);
      break;
    case 5:
      rrr_sample_rate = get_long(optarg);
      break;
    case 'c':
      request_count = get_long(optarg);
      break;
    case 'f':
      opt_force_rebuild = true;
      break;
    case 'h':
      print_help(stdout);
      break;
    case 'l':
      search_limit = get_long(optarg);
      break;
    case 'o':
      opt_inotify = false;
      break;
    case 'p':
      listen_port = get_long(optarg);
      break;
    case 'P':
      indexer_limit = get_long(optarg);
      break;
    case 'r':
      opt_recursive = true;
      break;
    case 's':
      data_suffix = optarg;
      break;
    case 'S':
      index_suffix = optarg;
      break;
    case 't':
      request_timeout = get_double(optarg);
      break;
    case '?':
      print_help(stderr);
      break;
    }
  }
  if (data_dir.empty())
    print_help(stderr);
  if (! indexer_limit) {
    indexer_limit = sysconf(_SC_NPROCESSORS_ONLN);
    if (indexer_limit < 0)
      err_exit(EX_OSERR, "sysconf");
  }

  RRRTable::init();

#define B(name) printf("%s: %s\n", #name, name ? "true" : "false")
#define D(name) printf("%s: %lg\n", #name, name)
#define I(name) printf("%s: %ld\n", #name, name)
#define S(name) printf("%s: %s\n", #name, name.c_str())
  puts(BOLD_CYAN "Data & index files:");
  B(opt_inotify);
  B(opt_recursive);
  S(data_suffix);
  S(index_suffix);
  I(indexer_limit);
  printf("data_dir:");
  for (auto dir: data_dir)
    printf(" %s", dir);
  puts("");

  puts("\nRequests:");
  I(autocomplete_length);
  I(autocomplete_limit);
  I(search_limit);
  D(request_timeout);

  puts("\nSuccinct data structures:");
  I(fmindex_sample_rate);
  I(rrr_sample_rate);

  printf(SGR0);
  fflush(stdout);

  Server::run();
}
