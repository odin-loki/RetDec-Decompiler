/* Complex control flow: state machine, nested conditions, multi-level break */
#include <stdio.h>
#include <string.h>
#include <ctype.h>

/* Lexer-style state machine (integer-only, no setjmp) */
typedef enum { S_NONE, S_WORD, S_NUM, S_SPACE, S_PUNCT } TokenType;

typedef struct {
    TokenType type;
    int start, len;
} Token;

int tokenize(const char *s, Token *toks, int max_toks) {
    int n = 0, i = 0;
    int slen = strlen(s);
    while (i < slen && n < max_toks) {
        if (isalpha(s[i])) {
            int start = i;
            while (i < slen && isalnum(s[i])) i++;
            toks[n++] = (Token){S_WORD, start, i - start};
        } else if (isdigit(s[i])) {
            int start = i;
            while (i < slen && isdigit(s[i])) i++;
            toks[n++] = (Token){S_NUM, start, i - start};
        } else if (isspace(s[i])) {
            while (i < slen && isspace(s[i])) i++;
            /* Don't emit space tokens */
        } else {
            toks[n++] = (Token){S_PUNCT, i, 1};
            i++;
        }
    }
    return n;
}

/* Multi-level break using flags */
int find_3d(int a[3][3][3], int val) {
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) {
            for (int k = 0; k < 3; k++) {
                if (a[i][j][k] == val)
                    return i * 9 + j * 3 + k;
            }
        }
    }
    return -1;
}

/* Complex if-else chain (if_structure_optimizer trigger) */
const char *classify_char(char c) {
    if (c >= 'A' && c <= 'Z') return "upper";
    else if (c >= 'a' && c <= 'z') return "lower";
    else if (c >= '0' && c <= '9') return "digit";
    else if (c == ' ' || c == '\t' || c == '\n') return "space";
    else if (c == '.' || c == ',' || c == '!' || c == '?') return "punct";
    else return "other";
}

/* Run-length encoding */
int rle_encode(const unsigned char *in, int in_len, unsigned char *out, int out_cap) {
    int out_len = 0;
    for (int i = 0; i < in_len; ) {
        unsigned char c = in[i];
        int count = 1;
        while (i + count < in_len && count < 255 && in[i + count] == c)
            count++;
        if (out_len + 2 > out_cap) return -1;
        out[out_len++] = (unsigned char)count;
        out[out_len++] = c;
        i += count;
    }
    return out_len;
}

int main(void) {
    const char *text = "Hello World! 123 test";
    Token toks[32];
    int n = tokenize(text, toks, 32);
    printf("tokens: %d\n", n);
    for (int i = 0; i < n; i++) {
        const char *type;
        switch (toks[i].type) {
            case S_WORD:  type = "WORD"; break;
            case S_NUM:   type = "NUM"; break;
            case S_PUNCT: type = "PUNCT"; break;
            default:      type = "?"; break;
        }
        printf("  [%d] %s len=%d\n", i, type, toks[i].len);
    }
    
    int cube[3][3][3];
    for (int i=0;i<3;i++) for(int j=0;j<3;j++) for(int k=0;k<3;k++) cube[i][j][k]=i*9+j*3+k;
    printf("find_3d(14)=%d\n", find_3d(cube, 14));
    
    for (const char *p = "Hello World! 123"; *p; p++)
        printf("'%c'=%s ", *p, classify_char(*p));
    printf("\n");
    
    unsigned char rle_in[] = "AAABBBCCDDDDDE";
    unsigned char rle_out[64];
    int rle_len = rle_encode(rle_in, strlen((char*)rle_in), rle_out, sizeof(rle_out));
    printf("rle_len=%d\n", rle_len);
    return 0;
}
