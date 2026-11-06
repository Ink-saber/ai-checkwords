/* ==========================================================================
 *  文件名 : wordfreq.c
 *  功能   : 命令行文本词频统计器（Windows 11 + MinGW-w64 gcc）
 *
 *  用法   : wordfreq.exe <指定文件路径> -n <数量>
 *  示例   : wordfreq.exe D:\data\article.txt -n 10
 *
 *  输出   : 序号. 单词 数量        （例如：1. 我 666）
 *
 *  本程序实现的全部规则（对应作业文档）：
 *   1. 数量只接受"阿拉伯数字组成的正整数"，其它任何写法都输出"数量有误"并结束；
 *   2. 一次最多输出 200 个词，请求的数量大于 200 时输出"数量超限"并结束；
 *   3. 文件不存在 / 打不开 / 路径错误，或根本不是文本文件（.docx/.xlsx 本质是
 *      ZIP 压缩包，.doc 是 OLE2 复合文档）-> "文件异常"；空白文件 -> "该文件为空"；
 *   4. 英文：不分大小写（The 和 the 是同一个词），按空格或标点切分单词，
 *      不是按单个字母统计；频次相同时按首字母 ASCII 码逐字符比较，若一方
 *      先结束（如 help 与 helpful），则短的一方排在前面；
 *   5. 中文：逐字统计（"星期五" -> 星 +1、期 +1、五 +1），频次相同的字按
 *      首次出现的先后排序（"红红火火" 中 红 在前、火 在后）；
 *   6. 特殊符号、阿拉伯数字一律不计入统计，只当作分隔符；
 *   7. 其他语言（俄语、德语等）：按空格/标点切出单词即可，不做大小写合并，
 *      频次相同时先出现的在前；
 *   8. 多语种文档中频次相同时：中文在前，英文其次，其余按首次出现排序；
 *   9. 源文件编码自动识别（UTF-8 / UTF-8 BOM / UTF-16 / GBK），控制台按
 *      UTF-8 输出，保证中文不乱码。
 *
 *  编译   : gcc -O2 -std=c11 -Wall -o wordfreq.exe wordfreq.c
 * ========================================================================== */

#define _CRT_SECURE_NO_WARNINGS

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>          /* 控制台代码页、多字节/宽字符转换等 Windows API */
#endif

/* 一次最多允许输出的词汇数量（作业要求：输出词汇最多不超过 200 个） */
#define MAX_OUTPUT_WORDS 200

/* 词条类别：用于"频次相同"时的跨语种排序（中文 -> 英文 -> 其他语言） */
enum { CAT_CN = 0, CAT_EN = 1, CAT_OTHER = 2 };

/* 一个词条：保存单词本身、类别、出现次数、首次出现的序号 */
typedef struct {
    char     *word;    /* 词的 UTF-8 文本（英文已统一转成小写），动态分配 */
    int       cat;     /* 类别，取值 CAT_CN / CAT_EN / CAT_OTHER */
    long long count;   /* 出现次数 */
    long long first;   /* 首次出现时的全局序号，用于"先出现的在前" */
} Entry;

/* 词频统计表：开放寻址法哈希表，避免逐个线性查找，处理大文件也很快 */
typedef struct {
    Entry    *slots;   /* 槽位数组，槽位的 word 为 NULL 表示空槽 */
    size_t    cap;     /* 槽位总数，始终是 2 的幂，可用位与代替取模 */
    size_t    size;    /* 已统计出的不同词条数 */
    long long seq;     /* 词条出现序号计数器（每处理一个词就 +1） */
    int       failed;  /* 内存分配失败标记 */
} WordTable;

/* --------------------------------------------------------------------------
 * 字符串小工具
 * ------------------------------------------------------------------------ */

/* 复制一个字符串（调用者负责 free） */
static char *dup_str(const char *s)
{
    size_t n = strlen(s);
    char *p = (char *)malloc(n + 1);
    if (p) memcpy(p, s, n + 1);
    return p;
}

/* --------------------------------------------------------------------------
 * 哈希表部分
 * ------------------------------------------------------------------------ */

/* FNV-1a 哈希；把类别也混进去，避免"同字节但不同类别"的键混在一起 */
static unsigned long hash_key(int cat, const char *s)
{
    unsigned long h = 2166136261u ^ ((unsigned long)(cat + 1) * 2654435761u);
    while (*s) {
        h ^= (unsigned char)*s++;
        h *= 16777619u;
    }
    return h;
}

/* 初始化统计表 */
static int table_init(WordTable *t)
{
    t->cap    = 1024;
    t->size   = 0;
    t->seq    = 0;
    t->failed = 0;
    t->slots  = (Entry *)calloc(t->cap, sizeof(Entry));
    return t->slots != NULL;
}

/* 释放统计表以及所有词条里的字符串 */
static void table_free(WordTable *t)
{
    if (!t->slots) return;
    for (size_t i = 0; i < t->cap; i++) free(t->slots[i].word);
    free(t->slots);
    t->slots = NULL;
}

/* 把词条放进指定的槽位数组（扩容重排时使用；字符串指针直接移交，不复制） */
static void table_put_raw(Entry *slots, size_t cap, const Entry *e)
{
    size_t idx = hash_key(e->cat, e->word) & (cap - 1);
    while (slots[idx].word != NULL)          /* 线性探测找空槽 */
        idx = (idx + 1) & (cap - 1);
    slots[idx] = *e;
}

/* 扩容：槽位翻倍并重新散列，负载因子超过 0.7 时调用 */
static int table_grow(WordTable *t)
{
    size_t ncap = t->cap * 2;
    Entry *nslots = (Entry *)calloc(ncap, sizeof(Entry));
    if (!nslots) return 0;

    for (size_t i = 0; i < t->cap; i++)
        if (t->slots[i].word)
            table_put_raw(nslots, ncap, &t->slots[i]);

    free(t->slots);
    t->slots = nslots;
    t->cap   = ncap;
    return 1;
}

/* 统计一个词：已存在则次数 +1，否则新建词条并记录首次出现序号 */
static void table_add(WordTable *t, const char *word, int cat)
{
    long long order = t->seq++;                    /* 本次出现的全局序号 */
    size_t idx = hash_key(cat, word) & (t->cap - 1);

    for (;;) {
        Entry *e = &t->slots[idx];

        if (e->word == NULL) {                     /* 空槽：新词条 */
            e->word = dup_str(word);
            if (!e->word) { t->failed = 1; return; }
            e->cat   = cat;
            e->count = 1;
            e->first = order;
            t->size++;

            if (t->size * 10 >= t->cap * 7) {      /* 负载因子达到 0.7，扩容 */
                if (!table_grow(t)) t->failed = 1;
            }
            return;
        }
        if (e->cat == cat && strcmp(e->word, word) == 0) {
            e->count++;                            /* 已统计过：次数 +1 */
            return;
        }
        idx = (idx + 1) & (t->cap - 1);            /* 线性探测下一个槽位 */
    }
}

/* --------------------------------------------------------------------------
 * UTF-8 与字符分类
 * ------------------------------------------------------------------------ */

/* 从 UTF-8 串中解出一个码点，返回其占用的字节数；非法序列返回 -1。
 * 调用方保证缓冲区尾部至少还有 8 个字节的 0，因此这里可以放心向前多看几个字节。 */
static int u8_next(const unsigned char *s, unsigned *cp)
{
    unsigned char c = s[0];

    if (c < 0x80) { *cp = c; return 1; }

    if ((c & 0xE0) == 0xC0) {                       /* 110xxxxx 10xxxxxx */
        if ((s[1] & 0xC0) != 0x80) return -1;
        *cp = ((unsigned)(c & 0x1F) << 6) | (unsigned)(s[1] & 0x3F);
        if (*cp < 0x80) return -1;                  /* 过长编码，非法 */
        return 2;
    }
    if ((c & 0xF0) == 0xE0) {                       /* 1110xxxx 10xxxxxx 10xxxxxx */
        if ((s[1] & 0xC0) != 0x80 || (s[2] & 0xC0) != 0x80) return -1;
        *cp = ((unsigned)(c & 0x0F) << 12) |
              ((unsigned)(s[1] & 0x3F) << 6) |
              (unsigned)(s[2] & 0x3F);
        if (*cp < 0x800) return -1;
        if (*cp >= 0xD800 && *cp <= 0xDFFF) return -1;   /* 代理区，非法 */
        return 3;
    }
    if ((c & 0xF8) == 0xF0) {                       /* 11110xxx 10xxxxxx ... */
        if ((s[1] & 0xC0) != 0x80 || (s[2] & 0xC0) != 0x80 ||
            (s[3] & 0xC0) != 0x80) return -1;
        *cp = ((unsigned)(c & 0x07) << 18) |
              ((unsigned)(s[1] & 0x3F) << 12) |
              ((unsigned)(s[2] & 0x3F) << 6) |
              (unsigned)(s[3] & 0x3F);
        if (*cp < 0x10000 || *cp > 0x10FFFF) return -1;
        return 4;
    }
    return -1;
}

/* 英文字母（ASCII） */
static int is_ascii_alpha(int c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

/* 汉字：基本区、扩展 A、兼容汉字、扩展 B 及以后的大字符集 */
static int is_cjk(unsigned cp)
{
    return (cp >= 0x4E00  && cp <= 0x9FFF)  ||   /* CJK 基本区（常用汉字） */
           (cp >= 0x3400  && cp <= 0x4DBF)  ||   /* 扩展 A */
           (cp >= 0xF900  && cp <= 0xFAFF)  ||   /* 兼容汉字 */
           (cp >= 0x20000 && cp <= 0x2FA1F);     /* 扩展 B ~ F */
}

/* 其他语言的字母：西里尔、希腊、拉丁扩展、假名、谚文等，按"单词"统计 */
static int is_other_letter(unsigned cp)
{
    if (cp >= 0x00C0 && cp <= 0x00FF && cp != 0x00D7 && cp != 0x00F7) return 1;
    if (cp >= 0x0100 && cp <= 0x024F) return 1;   /* 拉丁扩展 A/B */
    if (cp >= 0x0300 && cp <= 0x036F) return 1;   /* 组合用变音符号（属于单词的一部分） */
    if (cp >= 0x0370 && cp <= 0x03FF) return 1;   /* 希腊字母 */
    if (cp >= 0x0400 && cp <= 0x052F) return 1;   /* 西里尔字母（俄语等） */
    if (cp >= 0x0530 && cp <= 0x058F) return 1;   /* 亚美尼亚文 */
    if (cp >= 0x0590 && cp <= 0x05FF) return 1;   /* 希伯来文 */
    if (cp >= 0x0600 && cp <= 0x06FF) return 1;   /* 阿拉伯文 */
    if (cp >= 0x0900 && cp <= 0x097F) return 1;   /* 天城文 */
    if (cp >= 0x0E00 && cp <= 0x0E7F) return 1;   /* 泰文 */
    if (cp >= 0x1E00 && cp <= 0x1EFF) return 1;   /* 拉丁扩展附加 */
    if (cp >= 0x3040 && cp <= 0x30FF) return 1;   /* 平假名 / 片假名 */
    if (cp >= 0x31F0 && cp <= 0x31FF) return 1;
    if (cp >= 0xAC00 && cp <= 0xD7AF) return 1;   /* 谚文音节 */
    if (cp >= 0x1100 && cp <= 0x11FF) return 1;   /* 谚文字母 */
    return 0;
}

/* 判断整段字节是否严格合法的 UTF-8（用于自动识别文件编码） */
static int utf8_valid(const unsigned char *s, size_t n)
{
    size_t i = 0;

    while (i < n) {
        unsigned char c = s[i];

        if (c < 0x80) { i++; continue; }

        if ((c & 0xE0) == 0xC0) {
            if (i + 1 >= n || (s[i + 1] & 0xC0) != 0x80) return 0;
            if (c < 0xC2) return 0;                       /* C0/C1 过长编码 */
            i += 2; continue;
        }
        if ((c & 0xF0) == 0xE0) {
            if (i + 2 >= n) return 0;
            if ((s[i + 1] & 0xC0) != 0x80 || (s[i + 2] & 0xC0) != 0x80) return 0;
            unsigned cp = ((unsigned)(c & 0x0F) << 12) |
                          ((unsigned)(s[i + 1] & 0x3F) << 6) |
                          (unsigned)(s[i + 2] & 0x3F);
            if (cp < 0x800 || (cp >= 0xD800 && cp <= 0xDFFF)) return 0;
            i += 3; continue;
        }
        if ((c & 0xF8) == 0xF0) {
            if (i + 3 >= n) return 0;
            if ((s[i + 1] & 0xC0) != 0x80 || (s[i + 2] & 0xC0) != 0x80 ||
                (s[i + 3] & 0xC0) != 0x80) return 0;
            unsigned cp = ((unsigned)(c & 0x07) << 18) |
                          ((unsigned)(s[i + 1] & 0x3F) << 12) |
                          ((unsigned)(s[i + 2] & 0x3F) << 6) |
                          (unsigned)(s[i + 3] & 0x3F);
            if (cp < 0x10000 || cp > 0x10FFFF) return 0;
            i += 4; continue;
        }
        return 0;
    }
    return 1;
}

/* --------------------------------------------------------------------------
 * 可增长的字符串缓冲：用来拼接一个"单词"（英文/其他语言可能很长）
 * ------------------------------------------------------------------------ */
typedef struct {
    char  *buf;
    size_t len;
    size_t cap;
} StrBuf;

static int sb_append(StrBuf *b, const char *p, size_t n)
{
    if (b->len + n + 1 > b->cap) {
        size_t ncap = b->cap ? b->cap : 64;
        while (ncap < b->len + n + 1) ncap *= 2;
        char *nb = (char *)realloc(b->buf, ncap);
        if (!nb) return 0;
        b->buf = nb;
        b->cap = ncap;
    }
    memcpy(b->buf + b->len, p, n);
    b->len += n;
    b->buf[b->len] = '\0';
    return 1;
}

/* --------------------------------------------------------------------------
 * 分词与统计
 * ------------------------------------------------------------------------ */

/* 结束当前攒着的单词：纯 ASCII 字母的算英文（统一转小写），否则算其他语言 */
static void flush_word(WordTable *t, StrBuf *w)
{
    if (w->len == 0) return;

    int all_ascii = 1;
    for (size_t i = 0; i < w->len; i++) {
        unsigned char c = (unsigned char)w->buf[i];
        if (c >= 0x80 || !is_ascii_alpha(c)) { all_ascii = 0; break; }
    }

    if (all_ascii) {
        /* 英文：大小写不区分，统计与输出统一用小写形式 */
        for (size_t i = 0; i < w->len; i++)
            if (w->buf[i] >= 'A' && w->buf[i] <= 'Z')
                w->buf[i] = (char)(w->buf[i] - 'A' + 'a');
        table_add(t, w->buf, CAT_EN);
    } else {
        /* 其他语言：保持原样，不合并大小写 */
        table_add(t, w->buf, CAT_OTHER);
    }

    w->len = 0;
    if (w->buf) w->buf[0] = '\0';
}

/* 把整篇 UTF-8 文本切成"词"，逐个交给统计表 */
static void tokenize(const char *text, WordTable *t)
{
    const unsigned char *p = (const unsigned char *)text;
    StrBuf w = { NULL, 0, 0 };

    while (*p) {
        unsigned cp = 0;
        int n = u8_next(p, &cp);

        if (n < 0) {                    /* 非法字节：当作分隔符跳过 */
            p++;
            flush_word(t, &w);
            continue;
        }

        if (cp < 0x80) {
            if (is_ascii_alpha((int)cp)) {
                if (!sb_append(&w, (const char *)p, 1)) { t->failed = 1; break; }
            } else {
                /* 空格、标点、阿拉伯数字等：都是分隔符，本身不计入统计 */
                flush_word(t, &w);
            }
        } else if (is_cjk(cp)) {
            /* 中文逐字统计：先把前面攒着的英文/其他语言单词收尾 */
            flush_word(t, &w);
            char one[8];
            memcpy(one, p, (size_t)n);
            one[n] = '\0';
            table_add(t, one, CAT_CN);
        } else if (is_other_letter(cp)) {
            if (!sb_append(&w, (const char *)p, (size_t)n)) { t->failed = 1; break; }
        } else {
            /* 其他符号（全角标点、数学符号、emoji 等）：只当分隔符 */
            flush_word(t, &w);
        }

        p += n;
    }

    flush_word(t, &w);
    free(w.buf);
}

/* --------------------------------------------------------------------------
 * 文件读取与编码识别
 * ------------------------------------------------------------------------ */

/* 读入整个文件的原始字节；结尾额外补 8 个 0，方便 UTF-8 解码时越界读 */
static unsigned char *read_whole_file(FILE *fp, size_t *out_len)
{
    size_t cap = 1u << 16;
    size_t len = 0;
    unsigned char *buf = (unsigned char *)malloc(cap + 8);
    if (!buf) return NULL;

    for (;;) {
        if (len + 1 > cap) {                       /* 空间不够就翻倍 */
            size_t ncap = cap * 2;
            unsigned char *nb = (unsigned char *)realloc(buf, ncap + 8);
            if (!nb) { free(buf); return NULL; }
            buf = nb;
            cap = ncap;
        }
        size_t got = fread(buf + len, 1, cap - len, fp);
        len += got;
        if (got == 0) break;                       /* 读到文件末尾 */
    }

    if (ferror(fp)) { free(buf); return NULL; }
    memset(buf + len, 0, 8);                       /* 补 0，保证解码安全 */
    *out_len = len;
    return buf;
}

/* 用指定的 Windows 代码页把一段字节转换成 UTF-8（936 = GBK/ANSI，1200/1201 = UTF-16） */
static char *ansi_to_utf8(const char *src, size_t len, unsigned cp, size_t *out_len)
{
    char *out = NULL;

    if (len == 0) {                                /* 空内容直接返回空串 */
        out = (char *)calloc(1, 1);
        if (out) *out_len = 0;
        return out;
    }

    int wlen = MultiByteToWideChar((UINT)cp, 0, src, (int)len, NULL, 0);
    if (wlen <= 0) return NULL;

    wchar_t *wbuf = (wchar_t *)malloc(((size_t)wlen + 1) * sizeof(wchar_t));
    if (!wbuf) return NULL;
    MultiByteToWideChar((UINT)cp, 0, src, (int)len, wbuf, wlen);
    wbuf[wlen] = L'\0';

    int ulen = WideCharToMultiByte(CP_UTF8, 0, wbuf, wlen, NULL, 0, NULL, NULL);
    if (ulen > 0) {
        out = (char *)malloc((size_t)ulen + 8);
        if (out) {
            WideCharToMultiByte(CP_UTF8, 0, wbuf, wlen, out, ulen, NULL, NULL);
            out[ulen] = '\0';
            memset(out + ulen + 1, 0, 7);
            *out_len = (size_t)ulen;
        }
    }

    free(wbuf);
    return out;
}

/* 把 UTF-16 文本（已去掉 BOM）手工转换成 UTF-8，兼容代理对（emoji 等） */
static char *utf16_to_utf8(const unsigned char *src, size_t n, int little_endian, size_t *out_len)
{
    StrBuf b = { NULL, 0, 0 };
    size_t i = 0;

    while (i + 1 < n) {
        unsigned u = little_endian ? (unsigned)(src[i] | ((unsigned)src[i + 1] << 8))
                                   : (unsigned)(src[i + 1] | ((unsigned)src[i] << 8));
        i += 2;

        /* 遇到高位代理，尝试与后面的低位代理拼成一个 4 字节字符 */
        if (u >= 0xD800 && u <= 0xDBFF && i + 1 < n) {
            unsigned lo = little_endian ? (unsigned)(src[i] | ((unsigned)src[i + 1] << 8))
                                        : (unsigned)(src[i + 1] | ((unsigned)src[i] << 8));
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                u = 0x10000u + ((u - 0xD800u) << 10) + (lo - 0xDC00u);
                i += 2;
            }
        }

        char tmp[4];
        size_t m;
        if (u < 0x80) {
            tmp[0] = (char)u; m = 1;
        } else if (u < 0x800) {
            tmp[0] = (char)(0xC0 | (u >> 6));
            tmp[1] = (char)(0x80 | (u & 0x3F));
            m = 2;
        } else if (u < 0x10000) {
            tmp[0] = (char)(0xE0 | (u >> 12));
            tmp[1] = (char)(0x80 | ((u >> 6) & 0x3F));
            tmp[2] = (char)(0x80 | (u & 0x3F));
            m = 3;
        } else {
            tmp[0] = (char)(0xF0 | (u >> 18));
            tmp[1] = (char)(0x80 | ((u >> 12) & 0x3F));
            tmp[2] = (char)(0x80 | ((u >> 6) & 0x3F));
            tmp[3] = (char)(0x80 | (u & 0x3F));
            m = 4;
        }

        if (!sb_append(&b, tmp, m)) { free(b.buf); return NULL; }
    }

    if (!sb_append(&b, "", 1)) { free(b.buf); return NULL; }   /* 结尾补 '\0' */
    *out_len = b.len - 1;
    return b.buf;
}

/* 把命令行里的路径转成 UTF-8，只用于显示。
 * Windows 下 argv 是系统 ANSI 编码（中文系统即 GBK），而程序统一按 UTF-8 输出，
 * 若不转换，提示信息里的中文路径就会变成乱码。 */
static char *path_for_display(const char *path)
{
#ifdef _WIN32
    size_t n = 0;
    char *u = ansi_to_utf8(path, strlen(path), CP_ACP, &n);
    if (u) return u;
#endif
    return dup_str(path);
}

/* 判断路径是不是目录（目录也算"打不开的文件"） */
static int path_is_dir(const char *path)
{
#ifdef _WIN32
    DWORD attr = GetFileAttributesA(path);
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
#else
    (void)path;
    return 0;
#endif
}

/* 判断文件是不是"非文本"的二进制文件。
 * 说明：.docx / .xlsx / .pptx 的本质是 ZIP 压缩包，.doc / .xls 是 OLE2 复合文档，
 * 它们都不是纯文本，如果直接按文本解码，只会把压缩后的二进制字节拆成一堆乱字符，
 * 所以这里要先识别出来，避免统计出毫无意义的"单词"。 */
static int looks_binary(const unsigned char *raw, size_t n)
{
    /* 1) 按常见文件头识别（最可靠） */
    if (n >= 4 && raw[0] == 'P' && raw[1] == 'K' &&
        ((raw[2] == 0x03 && raw[3] == 0x04) ||      /* ZIP（docx/xlsx/pptx 都是它） */
         (raw[2] == 0x05 && raw[3] == 0x06) ||      /* 空 ZIP 包 */
         (raw[2] == 0x07 && raw[3] == 0x08))) {
        return 1;
    }
    if (n >= 4 && raw[0] == 0xD0 && raw[1] == 0xCF &&
        raw[2] == 0x11 && raw[3] == 0xE0) return 1; /* OLE2：旧版 .doc / .xls */
    if (n >= 5 && memcmp(raw, "%PDF-", 5) == 0) return 1;  /* PDF */
    if (n >= 2 && raw[0] == 0x1F && raw[1] == 0x8B) return 1; /* gzip */

    /* 2) 再看内容：ASCII 控制字符（制表、换行等正常空白除外）占比过高就是二进制。
     *    正常文本几乎没有控制字符，而二进制文件里 0x00~0x1F 能占到一成以上。 */
    size_t total = (n < 65536) ? n : 65536;          /* 只抽查开头 64 KB */
    size_t ctrl = 0;

    for (size_t i = 0; i < total; i++) {
        unsigned char c = raw[i];
        if ((c < 0x20 && c != '\t' && c != '\n' && c != '\r' && c != '\f') || c == 0x7F)
            ctrl++;
    }
    return total > 0 && ctrl * 100 > total * 2;      /* 超过 2% 判为二进制 */
}

/* 把文件读成 UTF-8 文本：自动识别 BOM / UTF-8 / GBK。
 * 返回 NULL 表示文件异常（不存在、打不开、是目录、读失败、不是文本文件）。
 * is_binary 为输出参数：当文件被判定为二进制文件时置 1，便于主函数给出提示。 */
static char *load_utf8_file(const char *path, size_t *out_len, int *is_binary)
{
    if (is_binary) *is_binary = 0;

    if (path_is_dir(path)) return NULL;

    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;

    size_t raw_len = 0;
    unsigned char *raw = read_whole_file(fp, &raw_len);
    fclose(fp);
    if (!raw) return NULL;

    char *text = NULL;

    /* 情况 1：带 UTF-8 BOM 的文本（记事本常这样保存） */
    if (raw_len >= 3 && raw[0] == 0xEF && raw[1] == 0xBB && raw[2] == 0xBF) {
        size_t m = raw_len - 3;
        text = (char *)malloc(m + 8);
        if (text) {
            memcpy(text, raw + 3, m);
            text[m] = '\0';
            memset(text + m + 1, 0, 7);
            *out_len = m;
        }
    }
    /* 情况 2：带 UTF-16 BOM 的文本（记事本的"Unicode"格式） */
    else if (raw_len >= 2 && ((raw[0] == 0xFF && raw[1] == 0xFE) ||
                              (raw[0] == 0xFE && raw[1] == 0xFF))) {
        int le = (raw[0] == 0xFF);                        /* FF FE = 小端，FE FF = 大端 */
        size_t m = (raw_len - 2) & ~(size_t)1;            /* 字节数必须为偶数 */
        text = utf16_to_utf8(raw + 2, m, le, out_len);
    }
    /* 情况 3：二进制文件（.docx、.xlsx、.pdf、图片、压缩包……）：不当作文本统计 */
    else if (looks_binary(raw, raw_len)) {
        if (is_binary) *is_binary = 1;
        free(raw);
        return NULL;
    }
    /* 情况 4：没有 BOM，按 UTF-8 校验，合法就原样使用 */
    else if (utf8_valid(raw, raw_len)) {
        text = (char *)malloc(raw_len + 8);
        if (text) {
            memcpy(text, raw, raw_len);
            text[raw_len] = '\0';
            memset(text + raw_len + 1, 0, 7);
            *out_len = raw_len;
        }
    }
    /* 情况 5：不是 UTF-8，按中文 Windows 的 ANSI 编码（GBK/936）处理 */
    else {
        text = ansi_to_utf8((const char *)raw, raw_len, 936u, out_len);
    }

    free(raw);
    return text;
}

/* --------------------------------------------------------------------------
 * 排序规则
 * ------------------------------------------------------------------------ */

/* 词条比较：返回负数表示 a 应排在 b 前面 */
static int cmp_entry(const void *pa, const void *pb)
{
    const Entry *a = (const Entry *)pa;
    const Entry *b = (const Entry *)pb;

    /* 1) 频次高的在前 */
    if (a->count != b->count) return (a->count > b->count) ? -1 : 1;

    /* 2) 频次相同：中文 -> 英文 -> 其他语言（跨语种排序规则） */
    if (a->cat != b->cat) return (a->cat < b->cat) ? -1 : 1;

    /* 3) 英文之间：按 ASCII 码逐字符比较；
     *    一方先结束（如 help 与 helpful）时 strcmp 会认为短词更小，正好短词在前 */
    if (a->cat == CAT_EN) {
        int r = strcmp(a->word, b->word);
        if (r != 0) return (r < 0) ? -1 : 1;
        return 0;
    }

    /* 4) 中文单字 / 其他语言：首次出现的在前 */
    if (a->first != b->first) return (a->first < b->first) ? -1 : 1;
    return 0;
}

/* --------------------------------------------------------------------------
 * 主函数
 * ------------------------------------------------------------------------ */

/* 解析数量参数：只接受由阿拉伯数字组成的正整数。
 * 数字过大时饱和处理（反正只要判断是否超过 200），避免整数溢出。 */
static int parse_count(const char *s, long long *out)
{
    long long v = 0;

    if (s == NULL || *s == '\0') return 0;

    for (const char *p = s; *p; p++) {
        if (*p < '0' || *p > '9') return 0;        /* 出现非数字字符：数量有误 */
        if (v > 1000000000LL) {
            v = 1000000000LL;                      /* 饱和，防止 overflow */
        } else {
            v = v * 10 + (*p - '0');
        }
    }
    if (v <= 0) return 0;                          /* 0 不是正整数 */

    *out = v;
    return 1;
}

int main(int argc, char *argv[])
{
#ifdef _WIN32
    /* 让控制台按 UTF-8 解释输出，中文才不会乱码 */
    SetConsoleOutputCP(CP_UTF8);
#endif

    long long n = 0;
    size_t len = 0;
    char *text = NULL;
    WordTable t;
    Entry *arr = NULL;
    int is_binary = 0;

    /* ---------- 1. 检查命令行格式：程序名 <文件路径> -n <数量> ---------- */
    if (argc != 4 || strcmp(argv[2], "-n") != 0) {
        printf("数量有误\n");
        char *p0 = path_for_display(argv[0]);
        fprintf(stderr, "用法: %s <指定文件路径> -n <数量>\n", p0 ? p0 : argv[0]);
        free(p0);
        return 1;
    }

    /* ---------- 2. 校验数量：必须是阿拉伯数字的正整数 ---------- */
    if (!parse_count(argv[3], &n)) {
        printf("数量有误\n");
        return 1;
    }

    /* 输出词汇最多 200 个，请求更多属于"数量超限" */
    if (n > MAX_OUTPUT_WORDS) {
        printf("数量超限\n");
        return 1;
    }

    /* ---------- 3. 读取文件并统一转成 UTF-8 ---------- */
    text = load_utf8_file(argv[1], &len, &is_binary);
    if (text == NULL) {
        printf("文件异常\n");
        if (is_binary) {            /* 提示放在 stderr，不影响标准输出的判定 */
            char *shown = path_for_display(argv[1]);
            fprintf(stderr,
                    "提示: \"%s\" 不是纯文本文件（.docx/.xlsx 等本质是 ZIP 压缩包），"
                    "请先另存为 .txt 再统计\n", shown ? shown : argv[1]);
            free(shown);
        }
        return 1;
    }
    if (len == 0) {                                /* 0 字节的空白文件 */
        printf("该文件为空\n");
        free(text);
        return 0;
    }

    /* 文本里偶尔会出现 NUL 字节（二进制残留），按空格处理，避免后面的内容被截断 */
    for (size_t i = 0; i < len; i++)
        if (text[i] == '\0') text[i] = ' ';

    /* ---------- 4. 分词并统计词频 ---------- */
    if (!table_init(&t)) {
        printf("内存不足\n");
        free(text);
        return 1;
    }
    tokenize(text, &t);
    free(text);

    if (t.failed) {
        printf("内存不足\n");
        table_free(&t);
        return 1;
    }
    if (t.size == 0) {                             /* 只有符号/数字/空白，没有可统计的词 */
        printf("该文件为空\n");
        table_free(&t);
        return 0;
    }

    /* ---------- 5. 排序并输出前 N 个（N 大于实际词数时输出全部） ---------- */
    arr = (Entry *)malloc(t.size * sizeof(Entry));
    if (!arr) {
        printf("内存不足\n");
        table_free(&t);
        return 1;
    }

    size_t k = 0;
    for (size_t i = 0; i < t.cap; i++)
        if (t.slots[i].word) arr[k++] = t.slots[i];

    qsort(arr, k, sizeof(Entry), cmp_entry);

    size_t show = ((size_t)n < k) ? (size_t)n : k;  /* 数量超过实际词数：输出全部 */
    for (size_t i = 0; i < show; i++)
        printf("%d. %s %lld\n", (int)(i + 1), arr[i].word, arr[i].count);

    free(arr);
    table_free(&t);
    return 0;
}
