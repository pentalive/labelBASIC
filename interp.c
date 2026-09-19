/* Label Basic interpreter
 *
 * Design assumptions (see README.md for full list):
 *  - Precedence low->high:  OR, AND, NOT, relational(< > == <= >= <>), + -, * / %, ^
 *  - STOP exits(0) immediately. END [var] exits(var) (0 if omitted).
 *  - MAT name(rows[,cols]) allocates a numeric 2D array (1D = rows x 1).
 *  - A line that is exactly "Identifier:" (col 1) is a label; every other
 *    non-blank, non-comment line is a statement.
 *  - Do / Next each may optionally carry a WHILE/UNTIL condition,
 *    independently:
 *      Do WHILE/UNTIL <expr>   - pre-test; if it fails, the whole loop body
 *                                is skipped (jump past matching Next).
 *      Next WHILE/UNTIL <expr> - post-test; if it holds (WHILE=true /
 *                                UNTIL=false... see semantics below) control
 *                                jumps back to just after the matching Do,
 *                                otherwise falls through past Next.
 *      A Do or Next with no condition just falls through / always loops
 *      back, respectively.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <time.h>
#include <unistd.h>
#include <locale.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <poll.h>

#define MAXLINES 8192
#define MAXVARS  2048
#define MAXARRS  128
#define MAXLABELS 1024
#define MAXCALLSTACK 512
#define MAXFUNCPARAMS 10
#define LINELEN 1024  /* also doubles as the max length of a single string
                          value (e.g. built via "+" concatenation) in the C
                          version -- longer results are silently truncated
                          by snprintf, not an error. The C++ version has no
                          such limit (std::string grows as needed). Chose to
                          leave this as-is rather than raise/remove it. */

/* ---------- program storage ---------- */
static char *g_lines[MAXLINES];
static int   g_nlines = 0;

typedef struct { char name[64]; int idx; } Label;
static Label g_labels[MAXLABELS];
static int   g_nlabels = 0;

static int g_doToNext[MAXLINES];
static int g_nextToDo[MAXLINES];
static int g_enclosingDo[MAXLINES]; /* innermost Do a line lexically sits inside, or -1 */

/* ---------- variables ---------- */
typedef struct {
    char name[64];
    int  isStr;
    double num;
    char *str;
} Var;
static Var g_vars[MAXVARS];
static int g_nvars = 0;

typedef struct {
    char name[64];
    int rows, cols;
    int isStr;      /* 1 for a $-prefixed (string) array, 0 for numeric */
    double *data;    /* used when isStr==0 */
    char **strdata;  /* used when isStr==1; each slot is an owned strdup'd string */
} Arr;
static Arr g_arrs[MAXARRS];
static int g_narrs = 0;

/* ---------- call stack ---------- */
typedef struct {
    int returnAddr;
    int savedIfFlag;      /* preserve caller's if flag across gosub/return */
} CallFrame;
static CallFrame g_callstack[MAXCALLSTACK];
static int g_callsp = 0;

/* ---------- condition flag for if/then/else ---------- */
static int g_ifFlag = -1;  /* -1 = no if, 0 = false, 1 = true */
static int g_ifLineNum = -1;  /* which line executed the if...then */

/* ---------- print format ---------- */
static char g_fmt[64] = "";  /* raw FORMAT string, e.g. "%+8.2L" */
static FILE *g_input = NULL; /* redirected input file (NULL = stdin) */
static int g_eof = 0;  /* set when ASK hits EOF on input stream */

/* ---------- current Print output target (OPEN) ---------- */
static FILE *g_out = NULL;      /* currently active target for PRINT: stdout, stderr, or an open file */
static FILE *g_outFile = NULL;  /* if non-NULL, a file WE opened, that we own and must fclose() */

static void closeOpenFileIfAny(void){
    if(g_outFile){ fclose(g_outFile); g_outFile=NULL; }
}

/* ---------- value type for expr eval ---------- */
typedef struct {
    int isStr;
    double num;
    char *str; /* owned, must be freed by caller when done, or NULL */
} Value;

static Value mkNum(double d){ Value v; v.isStr=0; v.num=d; v.str=NULL; return v; }
static Value mkStr(const char*s){ Value v; v.isStr=1; v.num=0; v.str=strdup(s?s:""); return v; }

/*    Number -> string with no padding:
**    integers print with no decimal point,
**    everything else uses %g (trims trailing zeros, no fixed width). Shared
**    by $str() and by PRINT's default (no active FORMAT) numeric output, so
**   they always agree.
*/
static void numToStr(double d, char *buf, size_t bufsz){
  if(g_fmt[0]){
    snprintf(buf, bufsz, g_fmt, d);
  } else {
    if(d==trunc(d) && fabs(d) < 1e15)
      snprintf(buf, bufsz, "%.0f", d);
    else
      snprintf(buf, bufsz, "%g", d);
  }
}

/* ================= utility ================= */ 

/*
** die() produce an error message and exit  
** Modified to make an error message that
** Emacs loves (that is parsed by emacs's compile
** command
*/
static void die(const char *msg, int lineno){
    /* lineno is 0-indexed in our array, but line 0 is the injected "GOTO BEGIN"
       so we display lineno (which shows user's actual line numbers) */
    fprintf(stderr, "LabelBasic: %s\n", msg);
    
    /* Print the offending line */
    if(lineno >= 0 && lineno < g_nlines && g_lines[lineno]){
        fprintf(stderr, "    %s\n", g_lines[lineno]);
    }
    
    exit(1);
}

static char *trim(char *s){
    while(*s==' '||*s=='\t') s++;
    char *end = s+strlen(s);
    while(end>s && (end[-1]=='\n'||end[-1]=='\r'||end[-1]==' '||end[-1]=='\t')) *--end=0;
    return s;
}

/* strip a # or ; comment (outside of quotes) */
static void stripComment(char *s){
    int inq=0;
    for(char *p=s; *p; p++){
        if(*p=='"') inq=!inq;
        else if((*p=='#' || *p==';') && !inq){ *p=0; break; }
    }
}

    /*
    ** A comment must start in col 1 (no leading space)
    **  with Ident: and nothing else meaningful after
    */

static int isLabelLine(const char *raw, char *nameOut){
    if(raw[0]==' '||raw[0]=='\t'||raw[0]==0) return 0;
    const char *p = raw;
    if(!(isalpha((unsigned char)*p) || *p=='_')) return 0;
    const char *start = p;
    while(isalnum((unsigned char)*p)||*p=='_') p++;
    if(*p != ':') return 0;
    int len = (int)(p-start);
    if(len<=0 || len>=64) return 0;
    memcpy(nameOut, start, len);
    nameOut[len]=0;
    p++; /* skip colon */
    while(*p==' '||*p=='\t') p++;
    return (*p==0); /* rest of line must be empty */
}

/* ================= variable / array storage ================= */
static Var* findVar(const char *name, int create){
    for(int i=0;i<g_nvars;i++) if(strcasecmp(g_vars[i].name,name)==0) return &g_vars[i];
    if(!create) return NULL;
    if(g_nvars>=MAXVARS){ fprintf(stderr,"Too many variables\n"); exit(1); }
    Var *v = &g_vars[g_nvars++];  /* why not use die here? */
    memset(v,0,sizeof(*v));
    strncpy(v->name,name,63);
    v->isStr = (name[0]=='$');
    return v;
}

static Arr* findArr(const char *name, int create){
    for(int i=0;i<g_narrs;i++) if(strcasecmp(g_arrs[i].name,name)==0) return &g_arrs[i];
    if(!create) return NULL;
    /* condsider die here too */
    if(g_narrs>=MAXARRS){ fprintf(stderr,"Too many arrays\n"); exit(1); }
    Arr *a = &g_arrs[g_narrs++];
    memset(a,0,sizeof(*a));
    strncpy(a->name,name,63);
    return a;
}

static void setNum(const char *name, double val){
    Var *v = findVar(name,1);
    v->isStr = 0;
    v->num = val;
}


static void setStr(const char *name, const char *val){
    Var *v = findVar(name,1);
    v->isStr = 1;
    free(v->str);
    v->str = strdup(val?val:"");
}

/* ================= expression parser ================= */
/* operates over a cursor into the current statement text */
typedef struct { const char *s; int pos; int lineno; } Cur;

static void skipws(Cur *c){ while(c->s[c->pos]==' '||c->s[c->pos]=='\t') c->pos++; }
static char peekc(Cur *c){ skipws(c); return c->s[c->pos]; }

static int matchWord(Cur *c, const char *word){
    skipws(c);
    int len = strlen(word);
    if(strncasecmp(c->s+c->pos, word, len)==0){
        char after = c->s[c->pos+len];
        if(isalnum((unsigned char)after)||after=='_') return 0;
        c->pos += len;
        return 1;
    }
    return 0;
}

static Value parseOr(Cur *c);

static Value evalArrRef(Arr *a, int r, int cIdx, int lineno){
  if(r<0||r>=a->rows||cIdx<0||cIdx>=a->cols) {
      die("array index out of range", lineno);
  }
  if(a->isStr) {
      return mkStr(a->strdata[r*a->cols+cIdx]);
  }

  return mkNum(a->data[r*a->cols+cIdx]);
} 

static Value parsePrimary(Cur *c){
    skipws(c);
    char ch = c->s[c->pos];

    if(ch=='('){
        c->pos++;
        Value v = parseOr(c);
        skipws(c);
        if(c->s[c->pos]==')') c->pos++;
        return v;
    }
    if(ch=='['){
        /* [expr] is grouping too, but unlike (expr) it's never mistaken for
           array indexing -- an identifier directly followed by '(' always
           means "index this array", with no way to mean "group" instead.
           [ ] sidesteps that in every position. */
        c->pos++;
        Value v = parseOr(c);
        skipws(c);
        if(c->s[c->pos]==']') c->pos++;
        return v;
    }
    if(ch=='"'){
        c->pos++;
        char buf[LINELEN]; int bi=0;
        while(c->s[c->pos] && c->s[c->pos] != '"'){
            buf[bi++] = c->s[c->pos++];
        }
        buf[bi]=0;
        if(c->s[c->pos]=='"') c->pos++;
        return mkStr(buf);
    }
    if(isdigit((unsigned char)ch) || (ch=='.' && isdigit((unsigned char)c->s[c->pos+1]))){
        char *end;
        double d = strtod(c->s+c->pos, &end);
        c->pos = (int)(end - c->s);
        return mkNum(d);
    }
    if(ch=='$' || isalpha((unsigned char)ch) || ch=='_'){
        int start = c->pos;
        if(ch=='$') c->pos++;
        while(isalnum((unsigned char)c->s[c->pos])||c->s[c->pos]=='_') c->pos++;
        char name[64]; int len = c->pos-start;
        if(len>=64) len=63;
        memcpy(name,c->s+start,len); name[len]=0;

        /* ternary(test, pass, fail) - conditional function */
        if(strcasecmp(name,"ternary")==0){
            skipws(c);
            if(c->s[c->pos]=='('){
                c->pos++;
                Value test = parseOr(c);
                skipws(c);
                if(c->s[c->pos]==',') c->pos++;
                Value pass = parseOr(c);
                skipws(c);
                if(c->s[c->pos]==',') c->pos++;
                Value fail = parseOr(c);
                skipws(c);
                if(c->s[c->pos]==')') c->pos++;
                /* if test is nonzero, return pass, else return fail */
                return (test.num != 0) ? pass : fail;
            }
        }

        /* EOF() - check if at end of input stream */
        if(strcasecmp(name,"EOF")==0){
            skipws(c);
            if(c->s[c->pos]=='(') c->pos++;
            skipws(c);
            if(c->s[c->pos]==')') c->pos++;
            return mkNum(g_eof);
        }

        /* single-argument functions */
        if(strcasecmp(name,"sin")==0 || strcasecmp(name,"cos")==0 || strcasecmp(name,"tan")==0 ||
           strcasecmp(name,"rnd")==0 || strcasecmp(name,"int")==0 || strcasecmp(name,"asc")==0 ||
           strcasecmp(name,"$chr")==0 || strcasecmp(name,"$str")==0){
            skipws(c);
            Value arg = mkNum(0);
            if(c->s[c->pos]=='('){
                c->pos++;
                arg = parseOr(c);
                skipws(c);
                if(c->s[c->pos]==')') c->pos++;
            }
            if(strcasecmp(name,"sin")==0) return mkNum(sin(arg.num));
            if(strcasecmp(name,"cos")==0) return mkNum(cos(arg.num));
            if(strcasecmp(name,"tan")==0) return mkNum(tan(arg.num));
            if(strcasecmp(name,"int")==0) return mkNum(trunc(arg.num));
            if(strcasecmp(name,"rnd")==0){
                int mx = (int)arg.num;
                if(mx<=0) mx=1;
                return mkNum((double)(rand()%mx));
            }
            if(strcasecmp(name,"asc")==0){
                if(arg.isStr && arg.str && arg.str[0]) return mkNum((double)(unsigned char)arg.str[0]);
                return mkNum(0);
            }
            if(strcasecmp(name,"$chr")==0){
                char buf[2]; buf[0]=(char)(int)arg.num; buf[1]=0;
                return mkStr(buf);
            }
            if(strcasecmp(name,"$str")==0){
                char buf[64];
                numToStr(arg.num, buf, sizeof(buf));
                return mkStr(buf);
            }
        }

        /* $front($string, $delimiter) - return part before delimiter
           if delimiter is empty: return first character
           if delimiter not found: return whole string */
        if(strcasecmp(name,"$front")==0){
            skipws(c);
            if(c->s[c->pos]=='('){
                c->pos++;
                Value str_val = parseOr(c);
                skipws(c);
                if(c->s[c->pos]==',') c->pos++;
                Value delim_val = parseOr(c);
                skipws(c);
                if(c->s[c->pos]==')') c->pos++;
                
                const char *str = str_val.isStr ? str_val.str : "";
                const char *delim = delim_val.isStr ? delim_val.str : "";
                
                if(!delim || delim[0]==0){
                    /* empty delimiter: return first char */
                    if(str[0]){
                        char buf[2]; buf[0]=str[0]; buf[1]=0;
                        return mkStr(buf);
                    }
                    return mkStr("");
                }
                
                /* find first occurrence of delimiter */
                const char *pos = strstr(str, delim);
                if(!pos){
                    /* delimiter not found: return whole string */
                    return mkStr(str);
                }
                /* return substring from start to delimiter */
                int len = (int)(pos - str);
                char buf[LINELEN];
                if(len >= LINELEN) len = LINELEN-1;
                strncpy(buf, str, len);
                buf[len] = 0;
                return mkStr(buf);
            }
        }

        /* $back($string, $delimiter) - return part after delimiter
           if delimiter is empty: return rest of string (skip first character)
           if delimiter not found: return whole string */
        if(strcasecmp(name,"$back")==0){
            skipws(c);
            if(c->s[c->pos]=='('){
                c->pos++;
                Value str_val = parseOr(c);
                skipws(c);
                if(c->s[c->pos]==',') c->pos++;
                Value delim_val = parseOr(c);
                skipws(c);
                if(c->s[c->pos]==')') c->pos++;
                
                const char *str = str_val.isStr ? str_val.str : "";
                const char *delim = delim_val.isStr ? delim_val.str : "";
                
                if(!delim || delim[0]==0){
                    /* empty delimiter: return rest of string (skip first char) */
                    if(str[0]){
                        return mkStr(str + 1);
                    }
                    return mkStr("");
                }
                
                /* find first occurrence of delimiter */
                const char *pos = strstr(str, delim);
                if(!pos){
                    /* delimiter not found: return whole string */
                    return mkStr(str);
                }
                /* return substring after delimiter */
                const char *after = pos + strlen(delim);
                return mkStr(after);
            }
        }

        /* $join($string1, $string2) - concatenate two strings */
        if(strcasecmp(name,"$join")==0){
            skipws(c);
            if(c->s[c->pos]=='('){
                c->pos++;
                Value str1_val = parseOr(c);
                skipws(c);
                if(c->s[c->pos]==',') c->pos++;
                Value str2_val = parseOr(c);
                skipws(c);
                if(c->s[c->pos]==')') c->pos++;
                
                const char *s1 = str1_val.isStr ? str1_val.str : "";
                const char *s2 = str2_val.isStr ? str2_val.str : "";
                
                char buf[LINELEN];
                snprintf(buf, sizeof(buf), "%s%s", s1, s2);
                return mkStr(buf);
            }
        }

        /* $key() - read a keystroke non-blocking, return descriptive string */
        if(strcasecmp(name,"$key")==0){
            skipws(c);
            if(c->s[c->pos]=='(') c->pos++;
            if(c->s[c->pos]==')') c->pos++;
            
            /* Check if stdin is a terminal */
            if(!isatty(STDIN_FILENO)){
                die("$key() requires interactive terminal", c->lineno);
            }
            
            /* Save original terminal settings */
            struct termios orig, raw;
            if(tcgetattr(STDIN_FILENO, &orig) < 0){
                die("$key() failed to get terminal attributes", c->lineno);
            }
            raw = orig;
            
            /* Disable canonical mode and echo */
            raw.c_lflag &= ~(ICANON | ECHO);
            raw.c_cc[VMIN] = 0;   /* non-blocking */
            raw.c_cc[VTIME] = 0;
            
            if(tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) < 0){
                die("$key() failed to set raw mode", c->lineno);
            }
            
            char result[64] = "";
            unsigned char ch;
            
            /* Read first character */
            if(read(STDIN_FILENO, &ch, 1) == 1){
                if(ch == 27){  /* ESC */
                    /* Try to read escape sequence */
                    unsigned char seq[10];
                    int seq_len = 0;
                    
                    /* Set a small timeout for reading the rest of the sequence */
                    struct pollfd pfd = {STDIN_FILENO, POLLIN, 0};
                    
                    while(seq_len < 9 && poll(&pfd, 1, 10) > 0){
                        if(read(STDIN_FILENO, &seq[seq_len], 1) != 1) break;
                        seq_len++;
                    }
                    
                    if(seq_len == 0){
                        /* Just ESC with nothing after */
                        strcpy(result, "Escape");
                    }
                    else if(seq[0] == '['){
                        /* CSI sequence: ESC [ ... */
                        if(seq_len >= 2){
                            /* Arrow keys: ESC [ A/B/C/D */
                            if(seq[1] >= 'A' && seq[1] <= 'D'){
                                const char *arrows[] = {"Up", "Down", "Right", "Left"};
                                strcpy(result, arrows[seq[1] - 'A']);
                            }
                            /* Function keys: ESC [ n ~ or ESC [ n n ~ 
                               Konsole uses: F1-F4: ESC O P/Q/R/S
                                            F5: ESC [ 1 5 ~
                                            F6: ESC [ 1 7 ~
                                            F7: ESC [ 1 8 ~
                                            F8: ESC [ 1 9 ~
                                            F9: ESC [ 2 0 ~
                                            F10: ESC [ 2 1 ~
                                            F11: ESC [ 2 3 ~
                                            F12: ESC [ 2 4 ~
                            */
                            else if(seq[1] >= '0' && seq[1] <= '9'){
                                int fkey = seq[1] - '0';
                                int idx = 2;
                                /* Handle multi-digit: collect all digits */
                                while(idx < seq_len && seq[idx] >= '0' && seq[idx] <= '9'){
                                    fkey = fkey * 10 + (seq[idx] - '0');
                                    idx++;
                                }
                                /* Map Konsole sequences to F-key numbers */
                                int mapped_fkey = fkey;
                                if(fkey == 15) mapped_fkey = 5;      /* ESC [ 1 5 ~ -> F5 */
                                else if(fkey == 17) mapped_fkey = 6; /* ESC [ 1 7 ~ -> F6 */
                                else if(fkey == 18) mapped_fkey = 7; /* ESC [ 1 8 ~ -> F7 */
                                else if(fkey == 19) mapped_fkey = 8; /* ESC [ 1 9 ~ -> F8 */
                                else if(fkey == 20) mapped_fkey = 9; /* ESC [ 2 0 ~ -> F9 */
                                else if(fkey == 21) mapped_fkey = 10;/* ESC [ 2 1 ~ -> F10 */
                                else if(fkey == 23) mapped_fkey = 11;/* ESC [ 2 3 ~ -> F11 */
                                else if(fkey == 24) mapped_fkey = 12;/* ESC [ 2 4 ~ -> F12 */
                                
                                if(mapped_fkey >= 1 && mapped_fkey <= 12){
                                    snprintf(result, sizeof(result), "F%d", mapped_fkey);
                                } else {
                                    /* Unrecognized sequence: return raw */
                                    snprintf(result, sizeof(result), "ESC[%d", fkey);
                                }
                            } else {
                                /* Unrecognized CSI sequence: return raw ESC[... */
                                snprintf(result, sizeof(result), "ESC[%c", seq[1]);
                            }
                        }
                    }
                    else if(seq[0] == 'O'){
                        /* SS3 sequence: ESC O P/Q/R/S/... */
                        if(seq_len >= 2){
                            if(seq[1] >= 'P' && seq[1] <= 'S'){
                                snprintf(result, sizeof(result), "F%d", 1 + (seq[1] - 'P'));
                            }
                            /* Some terminals: ESC O A/B/C/D for arrows */
                            else if(seq[1] >= 'A' && seq[1] <= 'D'){
                                const char *arrows[] = {"Up", "Down", "Right", "Left"};
                                strcpy(result, arrows[seq[1] - 'A']);
                            }
                        }
                    }
                } else if(ch == '\r' || ch == '\n'){
                    strcpy(result, "Enter");
                } else if(ch == 127 || ch == 8){  /* DEL or Backspace */
                    strcpy(result, "Backspace");
                } else if(ch == 9){  /* Tab */
                    strcpy(result, "Tab");
                } else if(ch < 32){  /* Control character */
                    snprintf(result, sizeof(result), "^%c", ch + 64);
                } else if(ch >= 32 && ch < 127){  /* Printable ASCII */
                    snprintf(result, sizeof(result), "%c", ch);
                }
            }
            
            /* Restore original terminal settings */
            tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig);
            
            return mkStr(result);
        }

        /* array reference: NAME(idx[,idx]) */
        skipws(c);
        if(c->s[c->pos]=='('){
            Arr *a = findArr(name,0);
            c->pos++;
            Value i1 = parseOr(c);
            int idx1 = (int)i1.num;
            int idx2 = 0;
            skipws(c);
            if(c->s[c->pos]==','){
                c->pos++;
                Value i2 = parseOr(c);
                idx2 = (int)i2.num;
            }
            skipws(c);
            if(c->s[c->pos]==')') c->pos++;
            if(!a) die("undeclared array (use MAT to declare)", c->lineno);
            return evalArrRef(a, idx1, idx2, c->lineno);
        }

        /* plain variable */
        Var *v = findVar(name,0);
        if(!v){
            /* undeclared var reads as 0 / "" */
            fprintf(stderr, "Warning: undefined variable '%s' at line %d, treating as 0\n", name, c->lineno);
            return name[0]=='$' ? mkStr("") : mkNum(0);
        }
        if(v->isStr) return mkStr(v->str?v->str:"");
        return mkNum(v->num);
    }
    die("unexpected token in expression", c->lineno);
    return mkNum(0);
}

static Value parseUnary(Cur *c){
    skipws(c);
    if(matchWord(c,"NOT")){
        /* NOT is unary, same tier as unary -/+, matching C's ! precedence:
           NOT a > b parses as (NOT a) > b, not NOT(a > b). */
        Value v = parseUnary(c);
        return mkNum(v.num==0 ? 1 : 0);
    }
    if(c->s[c->pos]=='-'){ c->pos++; Value v=parseUnary(c); return mkNum(-v.num); }
    if(c->s[c->pos]=='+'){ c->pos++; return parseUnary(c); }
    return parsePrimary(c);
}

static Value parseExponent(Cur *c){
    Value l = parseUnary(c);
    for(;;){
        skipws(c);
        if(c->s[c->pos]=='^'){
            c->pos++;
            Value r = parseUnary(c);
            double res = pow(l.num, r.num);
            l = mkNum(res);
        } else break;
    }
    return l;
}

static Value parseMul(Cur *c){
    Value l = parseExponent(c);
    for(;;){
        skipws(c);
        char op = c->s[c->pos];
        if(op=='*'||op=='/'||op=='%'){
            c->pos++;
            Value r = parseExponent(c);
            double res;
            if(op=='*') res = l.num*r.num;
            else if(op=='/') res = r.num!=0 ? l.num/r.num : 0;
            else res = r.num!=0 ? fmod(l.num,r.num) : 0;
            l = mkNum(res);
        } else break;
    }
    return l;
}

static Value parseAdd(Cur *c){
    Value l = parseMul(c);
    for(;;){
        skipws(c);
        char op = c->s[c->pos];
        if(op=='+'||op=='-'){
            /* don't consume if this is a unary sign belonging to a number in a bad spot;
               at this grammar level +/- here are always binary */
            c->pos++;
            Value r = parseMul(c);
            if(l.isStr || r.isStr){
                /* '+' on strings = concatenation; convenience beyond spec.
                   If one side is still a plain number (forgot $str()), convert
                   it rather than silently dropping it -- it used to fall back
                   to "" here, which quietly ate the number. */
                if(op=='+'){
                    char lbuf[64], rbuf[64];
                    const char *ls, *rs;
                    if(l.isStr) ls = l.str; else { numToStr(l.num, lbuf, sizeof(lbuf)); ls = lbuf; }
                    if(r.isStr) rs = r.str; else { numToStr(r.num, rbuf, sizeof(rbuf)); rs = rbuf; }
                    char buf[LINELEN]; /* result longer than LINELEN-1 chars is
                                           silently truncated by snprintf, not
                                           an error -- see LINELEN's comment. */
                    snprintf(buf,sizeof(buf),"%s%s",ls,rs);
                    l = mkStr(buf);
                    continue;
                }
            }
            l = mkNum(op=='+' ? l.num+r.num : l.num-r.num);
        } else break;
    }
    return l;
}

static int cmpValues(Value l, Value r){
    if(l.isStr || r.isStr){
        const char *ls = l.isStr? l.str:"";
        const char *rs = r.isStr? r.str:"";
        return strcmp(ls,rs);
    }
    if(l.num<r.num) return -1;
    if(l.num>r.num) return 1;
    return 0;
}

/* relational: < > <= >= (=>) -- binds tighter than equality, like C */
static Value parseRelational(Cur *c){
    Value l = parseAdd(c);
    skipws(c);
    if(strncmp(c->s+c->pos,"<>",2)==0) return l; /* not-equal belongs to the equality tier */
    if(strncmp(c->s+c->pos,"<=",2)==0){ c->pos+=2; Value r=parseAdd(c); return mkNum(cmpValues(l,r)<=0); }
    if(strncmp(c->s+c->pos,">=",2)==0 || strncmp(c->s+c->pos,"=>",2)==0){ c->pos+=2; Value r=parseAdd(c); return mkNum(cmpValues(l,r)>=0); }
    if(c->s[c->pos]=='<'){ c->pos++; Value r=parseAdd(c); return mkNum(cmpValues(l,r)<0); }
    if(c->s[c->pos]=='>'){ c->pos++; Value r=parseAdd(c); return mkNum(cmpValues(l,r)>0); }
    return l;
}

/* equality: == <> -- binds looser than relational, tighter than AND, like C's == != */
static Value parseEquality(Cur *c){
    Value l = parseRelational(c);
    for(;;){
        skipws(c);
        if(strncmp(c->s+c->pos,"==",2)==0){ c->pos+=2; Value r=parseRelational(c); l=mkNum(cmpValues(l,r)==0); }
        else if(strncmp(c->s+c->pos,"<>",2)==0){ c->pos+=2; Value r=parseRelational(c); l=mkNum(cmpValues(l,r)!=0); }
        else break;
    }
    return l;
}

static Value parseAnd(Cur *c){
    Value l = parseEquality(c);
    while(matchWord(c,"AND")){
        Value r = parseEquality(c);
        l = mkNum((l.num!=0 && r.num!=0)?1:0);
    }
    return l;
}

static Value parseOr(Cur *c){
    Value l = parseAnd(c);
    while(matchWord(c,"OR")){
        Value r = parseAnd(c);
        l = mkNum((l.num!=0 || r.num!=0)?1:0);
    }
    return l;
}

static Value evalExpr(const char *s, int lineno){
    Cur c; c.s=s; c.pos=0; c.lineno=lineno;
    return parseOr(&c);
}
/* also returns how far it consumed, for statements that need to know */
static Value evalExprAdv(const char *s, int lineno, int *consumed){
    Cur c; c.s=s; c.pos=0; c.lineno=lineno;
    Value v = parseOr(&c);
    if(consumed) *consumed = c.pos;
    return v;
}

/* ================= formatted printing ================= */

/* Validate that fmt has exactly one printf-style conversion for a double
   somewhere in it -- '%' [flags: + - 0 space #] [width] ['.' precision]
   [f|F|e|E|g|G] -- with any other literal text (including surrounding
   spaces/words, and "%%" as a literal percent) allowed freely around it.
   Rejected: zero conversions, more than one (would need more than the
   single double argument we pass), or '%' followed by anything that
   isn't a valid float conversion or a "%%" escape (e.g. %d, %s -- those
   would be a type mismatch with the double argument, which is undefined
   behavior in printf). */
static int isValidDoubleFormat(const char *fmt){
    int convCount = 0;
    const char *f = fmt;
    while(*f){
        if(*f=='%'){
            if(f[1]=='%'){ f += 2; continue; } /* literal %% escape */
            const char *p = f+1;
            while(*p=='+'||*p=='-'||*p=='0'||*p==' '||*p=='#'||*p=='\'') p++;
            while(isdigit((unsigned char)*p)) p++;
            if(*p=='.'){ p++; while(isdigit((unsigned char)*p)) p++; }
            if(*p=='f'||*p=='F'||*p=='e'||*p=='E'||*p=='g'||*p=='G'){
                p++;
                convCount++;
                f = p;
                continue;
            }
            return 0; /* '%' followed by something that isn't a valid float conversion or %% */
        }
        f++;
    }
    return convCount==1;
}

static void printNum(double d){
    if(g_fmt[0]==0){
        char buf[64];
        numToStr(d, buf, sizeof(buf));
        fprintf(g_out, "%s", buf);
        return;
    }
    /* g_fmt was already validated when the FORMAT statement ran, so it's
       safe to hand straight to printf as a real printf spec for a double. */
    fprintf(g_out, g_fmt, d);
}


/* ================= array declaration ================= */
static void freeArrStorage(Arr *a){
    if(a->isStr && a->strdata){
        for(int i=0;i<a->rows*a->cols;i++) free(a->strdata[i]);
        free(a->strdata);
        a->strdata = NULL;
    } else {
        free(a->data);
        a->data = NULL;
    }
}

static void declareArr(const char *name, int rows, int cols){
    Arr *a = findArr(name,1);
    freeArrStorage(a);
    if(rows<1) rows=1;
    if(cols<1) cols=1;
    a->rows=rows; a->cols=cols;
    a->isStr = (name[0]=='$');
    if(a->isStr){
        a->strdata = calloc((size_t)rows*cols, sizeof(char*));
        for(int i=0;i<rows*cols;i++) a->strdata[i] = strdup("");
    } else {
        a->data = calloc((size_t)rows*cols, sizeof(double));
    }
}

/* ================= assignment (array-aware) ================= */
static void assignTo(Cur *c, Value rhs, int lineno){
    skipws(c);
    int start=c->pos;
    if(!(c->s[c->pos]=='$'||isalpha((unsigned char)c->s[c->pos])||c->s[c->pos]=='_'))
        die("expected variable name after LET", lineno);
    if(c->s[c->pos]=='$') c->pos++;
    while(isalnum((unsigned char)c->s[c->pos])||c->s[c->pos]=='_') c->pos++;
    char name[64]; int len=c->pos-start; if(len>=64) len=63;
    memcpy(name,c->s+start,len); name[len]=0;

    skipws(c);
    if(c->s[c->pos]=='('){
        Arr *a = findArr(name,0);
        if(!a) die("undeclared array (use MAT to declare)", lineno);
        c->pos++;
        Value i1 = parseOr(c);
        int idx1=(int)i1.num, idx2=0;
        skipws(c);
        if(c->s[c->pos]==','){ c->pos++; Value i2=parseOr(c); idx2=(int)i2.num; }
        skipws(c);
        if(c->s[c->pos]==')') c->pos++;
        if(idx1<0||idx1>=a->rows||idx2<0||idx2>=a->cols) die("array index out of range", lineno);
        int slot = idx1*a->cols+idx2;
        if(a->isStr){
            char buf[64];
            const char *s;
            if(rhs.isStr) s = rhs.str;
            else { numToStr(rhs.num, buf, sizeof(buf)); s = buf; } /* auto-convert, don't drop */
            free(a->strdata[slot]);
            a->strdata[slot] = strdup(s);
        } else {
            a->data[slot] = rhs.isStr ? atof(rhs.str) : rhs.num; /* auto-convert, don't drop */
        }
        return;
    }
    if(name[0]=='$'){
        if(rhs.isStr) setStr(name, rhs.str);
        else { char buf[64]; numToStr(rhs.num, buf, sizeof(buf)); setStr(name, buf); } /* auto-convert, don't drop */
    }
    else setNum(name, rhs.isStr ? atof(rhs.str) : rhs.num); /* auto-convert string to number */
}

/* ================= program loading / pre-scan ================= */
static void loadProgram(const char *path){
    FILE *f = fopen(path,"r");
    if(!f){ fprintf(stderr,"cannot open %s\n",path); exit(1); }
    
    /* First line: automatic GOTO BEGIN to enable include files with subroutines */
    if(g_nlines>=MAXLINES){ fprintf(stderr,"program too large\n"); exit(1); }
    g_lines[g_nlines++] = strdup("GOTO BEGIN");
    
    char buf[LINELEN];
    while(fgets(buf,sizeof(buf),f)){
        stripComment(buf);
        char *t = trim(buf);
        if(g_nlines>=MAXLINES){ fprintf(stderr,"program too large\n"); exit(1); }
        g_lines[g_nlines++] = strdup(t);
    }
    fclose(f);
}

static void prescan(void){
    for(int i=0;i<g_nlines;i++){ g_doToNext[i]=-1; g_nextToDo[i]=-1; g_enclosingDo[i]=-1; }
    /* labels */
    for(int i=0;i<g_nlines;i++){
        char name[64];
        if(isLabelLine(g_lines[i], name)){
            if(g_nlabels>=MAXLABELS){ fprintf(stderr,"too many labels\n"); exit(1); }
            strncpy(g_labels[g_nlabels].name, name, 63);
            g_labels[g_nlabels].idx = i+1; /* label points at line AFTER it */
            g_nlabels++;
        }
    }
    /* match Do/Next by simple stack (line order); also record, for every
       line, the innermost Do it's lexically inside (needed by Break). */
    int stack[MAXLINES]; int sp=0;
    for(int i=0;i<g_nlines;i++){
        const char *l = g_lines[i];
        g_enclosingDo[i] = (sp>0) ? stack[sp-1] : -1;
        if(strncasecmp(l,"Do",2)==0 && (l[2]==0||l[2]==' '||l[2]=='\t')){
            stack[sp++]=i;
        } else if(strncasecmp(l,"Next",4)==0 && (l[4]==0||l[4]==' '||l[4]=='\t')){
            if(sp==0){ fprintf(stderr,"Next without matching Do at line %d\n", i+1); exit(1); }
            int di = stack[--sp];
            g_doToNext[di]=i;
            g_nextToDo[i]=di;
        }
    }
}

static int findLabel(const char *name){
    for(int i=0;i<g_nlabels;i++) if(strcasecmp(g_labels[i].name,name)==0) return g_labels[i].idx;
    return -1;
}

/* read one identifier-ish token (label name / var name) starting at cursor, no eval */
static void readIdent(Cur *c, char *out, int outsz){
    skipws(c);
    int start=c->pos;
    if(c->s[c->pos]=='$') c->pos++;
    while(isalnum((unsigned char)c->s[c->pos])||c->s[c->pos]=='_') c->pos++;
    int len=c->pos-start; if(len>=outsz) len=outsz-1;
    memcpy(out,c->s+start,len); out[len]=0;
}

/* Xtend Hue's FG/BG argument: a recognized color name (full word or the same
   3-letter abbreviation colorgrid.lbas uses), or -- if neither matches --
   fall back to a general numeric expression, so variables/loop counters
   work too (Xtend Hue fg bg). */
static int readColorArg(Cur *c){
    if(matchWord(c,"BLACK") || matchWord(c,"BLK")) return 0;
    if(matchWord(c,"RED")) return 1;
    if(matchWord(c,"GREEN") || matchWord(c,"GRN")) return 2;
    if(matchWord(c,"YELLOW") || matchWord(c,"YEL")) return 3;
    if(matchWord(c,"BLUE") || matchWord(c,"BLU")) return 4;
    if(matchWord(c,"MAGENTA") || matchWord(c,"MAG")) return 5;
    if(matchWord(c,"CYAN") || matchWord(c,"CYN")) return 6;
    if(matchWord(c,"WHITE") || matchWord(c,"WHT")) return 7;
    Value v = parseOr(c);
    return (int)v.num;
}

/* ================= statement execution ================= */
/* returns next pc (default caller does pc+1 if this returns -1 meaning "fallthrough") */
static int execStatement(const char *raw,int pc);

static int execLine(int pc){
    return execStatement(g_lines[pc], pc);
}

static int execStatement(const char *raw,int pc){
    char name[64];
    if(raw[0]==0) return -1; /* blank line */
    if(isLabelLine(raw,name)) return -1; /* label line, no-op */

    Cur c; c.s=raw; c.pos=0; c.lineno=pc;

    /* ! statement - execute a shell command synchronously */
    if(c.s[c.pos]=='!'){
        c.pos++;
        skipws(&c);
        Value cmdVal = parseOr(&c);
        const char *cmd = cmdVal.isStr ? cmdVal.str : "";
        if(cmd && cmd[0]){
            int ret = system(cmd);
            (void)ret; /* suppress unused warning */
        }
        return -1;
    }

    if(matchWord(&c,"ASK")){
        skipws(&c);
        FILE *inputSrc = g_input ? g_input : stdin;
        if(c.s[c.pos]=='"'){
            c.pos++;
            char prompt[LINELEN]; int pi=0;
            while(c.s[c.pos] && c.s[c.pos]!='"') prompt[pi++]=c.s[c.pos++];
            prompt[pi]=0;
            if(c.s[c.pos]=='"') c.pos++;
            /* only print prompt if reading from stdin */
            if(!g_input){
                printf("%s", prompt);
                fflush(stdout);
            }
        }
        skipws(&c);
        if(c.s[c.pos]==',') c.pos++;
        
        /* count how many variables are in the ASK statement */
        Cur varCounter = c;
        int varCount = 0;
        for(;;){
            skipws(&varCounter);
            if(varCounter.s[varCounter.pos]==0) break;
            char dummy[64];
            readIdent(&varCounter, dummy, sizeof(dummy));
            if(dummy[0]==0) break;
            varCount++;
            skipws(&varCounter);
            if(varCounter.s[varCounter.pos]==',') {
                varCounter.pos++;
            } else {
                break;
            }
        }
        
        char line[LINELEN];
        if(!fgets(line,sizeof(line),inputSrc)){
            line[0]=0;
            g_eof = 1;
        } else {
            g_eof = 0;
        }
        line[strcspn(line,"\r\n")]=0;
        
        /* split input into tokens */
        char *inputCopy = strdup(line);
        char *tokens[MAXFUNCPARAMS];
        int tokenCount = 0;
        
        if(varCount > 1){
            /* split on commas - trim each token for cleanliness */
            char *tok = strtok(inputCopy, ",");
            while(tok && tokenCount < MAXFUNCPARAMS){
                tokens[tokenCount++] = trim(tok);
                tok = strtok(NULL, ",");
            }
        } else {
            /* single variable: preserve formatting, don't trim */
            tokens[0] = inputCopy;
            tokenCount = 1;
        }
        
        /* assign to variables */
        int tokenIdx = 0;
        for(;;){
            skipws(&c);
            if(c.s[c.pos]==0) break;
            char vname[64];
            readIdent(&c, vname, sizeof(vname));
            if(vname[0]==0) break;
            char *val = (tokenIdx < tokenCount) ? tokens[tokenIdx] : "";
            if(vname[0]=='$') setStr(vname, val);
            else setNum(vname, atof(val));
            tokenIdx++;
            skipws(&c);
            if(c.s[c.pos]==',') c.pos++;
            else break;
        }
        free(inputCopy);
        return -1;
    }

    if(matchWord(&c,"Call")){
        char lbl[64]; readIdent(&c,lbl,sizeof(lbl));
        int target = findLabel(lbl);
        if(target<0) die("unknown label in Call", pc);
        if(g_callsp>=MAXCALLSTACK) die("call stack overflow", pc);
        g_callstack[g_callsp].returnAddr = pc+1;
        g_callstack[g_callsp].savedIfFlag = g_ifFlag;  /* save if flag */
        g_callsp++;
        return target;
    }

    if(matchWord(&c,"Return") || matchWord(&c,"Returnd")){
        if(g_callsp==0) die("Return with empty call stack", pc);
        int retAddr = g_callstack[g_callsp-1].returnAddr;
        g_ifFlag = g_callstack[g_callsp-1].savedIfFlag;  /* restore if flag */
        g_callsp--;
        return retAddr;
    }

    if(matchWord(&c,"Goto")){
        char lbl[64]; readIdent(&c,lbl,sizeof(lbl));
        int target = findLabel(lbl);
        if(target<0) die("unknown label in Goto", pc);
        return target;
    }

    if(matchWord(&c,"If")){
        Value cond = parseOr(&c);
        skipws(&c);
        if(!matchWord(&c,"Then")) die("expected THEN in If", pc);
        skipws(&c);
        /* Always set the flag (for else to check later) */
        g_ifFlag = (cond.num != 0) ? 1 : 0;
        g_ifLineNum = pc;
        if(c.s[c.pos]==0) return -1;  /* no statement after then */
        if(cond.num==0) return -1;     /* condition false: skip then-statement */
        return execStatement(c.s + c.pos, pc);  /* condition true: execute then-statement */
    }

    if(matchWord(&c,"Else")){
        if(g_ifFlag==-1 || g_ifLineNum==-1) die("Else without If...Then", pc);
        if(g_ifLineNum != pc-1) die("Else not immediately after If...Then", pc);
        if(g_ifFlag==1) return -1;  /* if condition was true: skip else */
        skipws(&c);
        if(c.s[c.pos]==0) return -1;  /* no statement after else */
        return execStatement(c.s + c.pos, pc);  /* condition was false: execute else-statement */
    }

    if(matchWord(&c,"Let")){
        skipws(&c);
        int save=c.pos;
        /* find '=' that isn't part of == */
        /* simplest: locate var name, then require '=' */
        char vname[64];
        int nstart=c.pos;
        if(c.s[c.pos]=='$') c.pos++;
        while(isalnum((unsigned char)c.s[c.pos])||c.s[c.pos]=='_') c.pos++;
        int len=c.pos-nstart; if(len>=64) len=63;
        memcpy(vname,c.s+nstart,len); vname[len]=0;
        /* allow array target: NAME(idx[,idx]) */
        int isArrTarget=0; int savePos=c.pos;
        skipws(&c);
        if(c.s[c.pos]=='(') isArrTarget=1;
        c.pos = save; /* rewind, let assignTo re-parse name+optional index */
        skipws(&c);
        /* re-scan to '=' */
        Cur c2 = c;
        /* move c2 past name (and array index if present) to find '=' */
        if(c2.s[c2.pos]=='$') c2.pos++;
        while(isalnum((unsigned char)c2.s[c2.pos])||c2.s[c2.pos]=='_') c2.pos++;
        if(isArrTarget){
            skipws(&c2);
            if(c2.s[c2.pos]=='('){
                int depth=0;
                do{
                    if(c2.s[c2.pos]=='(') depth++;
                    else if(c2.s[c2.pos]==')') depth--;
                    c2.pos++;
                } while(c2.s[c2.pos] && depth>0);
            }
        }
        skipws(&c2);
        if(c2.s[c2.pos]!='=') die("expected '=' in Let", pc);
        c2.pos++;
        Value rhs = parseOr(&c2);
        (void)savePos;
        assignTo(&c, rhs, pc);
        return -1;
    }

    if(matchWord(&c,"Mat")){
        char aname[64]; readIdent(&c,aname,sizeof(aname));
        skipws(&c);
        int rows=1, cols=1;
        if(c.s[c.pos]=='('){
            c.pos++;
            Value r = parseOr(&c);
            rows=(int)r.num;
            skipws(&c);
            if(c.s[c.pos]==','){
                c.pos++;
                Value cc = parseOr(&c);
                cols=(int)cc.num;
            }
            skipws(&c);
            if(c.s[c.pos]==')') c.pos++;
        }
        declareArr(aname, rows, cols);
        return -1;
    }

    if(matchWord(&c,"Print")){
        int first=1;
        int addNewline=1;  /* set to 0 if NOCR() is encountered */
        for(;;){
            skipws(&c);
            if(c.s[c.pos]==0) break;
            
            /* Check for NOCR() flag */
            if(matchWord(&c,"Nocr")){
                skipws(&c);
                if(c.s[c.pos]=='('){
                    c.pos++;
                    skipws(&c);
                    if(c.s[c.pos]==')'){
                        c.pos++;
                        addNewline=0;  /* suppress the newline */
                        skipws(&c);
                        if(c.s[c.pos]==',') c.pos++;  /* consume optional comma */
                        continue;
                    }
                }
                die("expected NOCR()", pc);
            }
            
            Value v = parseOr(&c);
            if(!first) fprintf(g_out, "");
            first=0;
            if(v.isStr) fprintf(g_out, "%s", v.str? v.str:"");
            else printNum(v.num);
            skipws(&c);
            if(c.s[c.pos]==','){ c.pos++; continue; }
            break;
        }
        if(addNewline) fprintf(g_out, "\n");
        return -1;
    }

    if(matchWord(&c,"Open")){
        skipws(&c);
        if(c.s[c.pos]!='"') die("Open expects a quoted argument", pc);
        c.pos++;
        char arg1[LINELEN]; int ai=0;
        while(c.s[c.pos] && c.s[c.pos]!='"') arg1[ai++]=c.s[c.pos++];
        arg1[ai]=0;
        if(c.s[c.pos]=='"') c.pos++;
        skipws(&c);
        if(c.s[c.pos]==','){
            /* Open "filename", w|a|r */
            c.pos++;
            skipws(&c);
            char mode[16]; int mi=0;
            while(isalpha((unsigned char)c.s[c.pos]) && mi<15) mode[mi++]=c.s[c.pos++];
            mode[mi]=0;
            if(strcasecmp(mode,"w")==0 || strcasecmp(mode,"a")==0){
                /* output redirection */
                FILE *f = fopen(arg1, strcasecmp(mode,"w")==0 ? "w" : "a");
                if(!f) die("could not open file for Open", pc);
                closeOpenFileIfAny();
                g_outFile = f;
                g_out = f;
            } else if(strcasecmp(mode,"r")==0){
                /* input redirection */
                if(g_input && g_input != stdin) fclose(g_input);
                FILE *f = fopen(arg1, "r");
                if(!f) die("could not open file for reading", pc);
                g_input = f;
            } else {
                die("Open mode must be w, a, or r", pc);
            }
        } else {
            /* Open "ERR", "OUT", or "INP" -- switch target, closing any open file */
            closeOpenFileIfAny();
            if(strcasecmp(arg1,"ERR")==0) g_out = stderr;
            else if(strcasecmp(arg1,"OUT")==0) g_out = stdout;
            else if(strcasecmp(arg1,"INP")==0){
                /* close input file and return to stdin */
                if(g_input && g_input != stdin) fclose(g_input);
                g_input = NULL;
            }
            else die("Open without a mode must be \"ERR\", \"OUT\", or \"INP\"", pc);
        }
        return -1;
    }

    if(matchWord(&c,"Xtend")){
        for(;;){
            skipws(&c);
            if(c.s[c.pos]==0) break; /* end of line: no more sub-commands */
            if(matchWord(&c,"Hue")){
                int fg = readColorArg(&c);
                int bg = readColorArg(&c);
                fprintf(g_out, "%c[%d;%dm", 27, 30+fg, 40+bg);
            } else if(matchWord(&c,"CLS")){
                fprintf(g_out, "%c[2J", 27);
            } else if(matchWord(&c,"HOME")){
                fprintf(g_out, "%c[H", 27);
            } else if(matchWord(&c,"At")){
                Value row = parseOr(&c);
                Value col = parseOr(&c);
                fprintf(g_out, "%c[%d;%dH", 27, (int)row.num, (int)col.num);
            } else {
                die("unknown Xtend sub-command (expected Hue, CLS, HOME, or At)", pc);
            }
            skipws(&c);
            if(c.s[c.pos]==',') c.pos++; /* optional comma between chained sub-commands */
        }
        return -1;
    }

    if(matchWord(&c,"Format")){
        skipws(&c);
        if(c.s[c.pos]=='"'){
            c.pos++;
            int fi=0;
            while(c.s[c.pos] && c.s[c.pos]!='"') g_fmt[fi++]=c.s[c.pos++];
            g_fmt[fi]=0;
            if(c.s[c.pos]=='"') c.pos++;
            if(!isValidDoubleFormat(g_fmt))
                die("FORMAT string must be a printf-style spec for a double, e.g. \"%+8.2f\"", pc);
        }
        return -1;
    }

    if(matchWord(&c,"Break")){
        int doIdx = g_enclosingDo[pc];
        skipws(&c);
        int hasCond = matchWord(&c,"If");
        Value cond;
        if(hasCond) cond = parseOr(&c);
        if(doIdx<0) die("Break outside of a loop", pc); /* structural error: always checked, even if the condition is false */
        if(hasCond && cond.num==0) return -1; /* condition false: fall through, no break */
        int nextIdx = g_doToNext[doIdx];
        if(nextIdx<0) die("Break's loop has no matching Next", pc);
        return nextIdx+1;
    }

    if(matchWord(&c,"Do")){
        skipws(&c);
        int isWhile=-1;
        if(matchWord(&c,"While")) isWhile=1;
        else if(matchWord(&c,"Until")) isWhile=0;
        if(isWhile>=0){
            Value cond = parseOr(&c);
            int truthy = cond.num!=0;
            int shouldSkip = isWhile ? !truthy : truthy;
            if(shouldSkip){
                int nextIdx = g_doToNext[pc];
                if(nextIdx<0) die("Do has no matching Next", pc);
                return nextIdx+1;
            }
        }
        return -1;
    }

    if(matchWord(&c,"Next")){
        skipws(&c);
        int isWhile=-1;
        int doIdx = g_nextToDo[pc];
        if(doIdx<0) die("Next without matching Do", pc);
        if(matchWord(&c,"While")) isWhile=1;
        else if(matchWord(&c,"Until")) isWhile=0;
        if(isWhile<0){
            /* unconditional: always loop back, let Do re-test its own condition */
            return doIdx;
        }
        Value cond = parseOr(&c);
        int truthy = cond.num!=0;
        int loopBack = isWhile ? truthy : !truthy;
        return loopBack ? doIdx : -1;
    }

    if(matchWord(&c,"End")){
        skipws(&c);
        int code=0;
        if(c.s[c.pos]) { Value v = parseOr(&c); code=(int)v.num; }
        exit(code);
    }

    if(matchWord(&c,"Stop")){
        exit(0);
    }

    die("unrecognized statement", pc);
    return -1;
}

/* ================= main ================= */
int main(int argc, char **argv){
    if(argc<2){ fprintf(stderr,"usage: %s program.lbas\n", argv[0]); return 1; }
    /* Adopt the environment's locale for numeric formatting (LANG/LC_NUMERIC/
       LC_ALL) so printf's glibc "'" thousands-grouping flag (%'.2f) actually
       has a grouping character to use -- without this call, the C library
       defaults to the "C" locale, which defines no grouping at all, and the
       flag silently does nothing. NOTE: this also means the decimal POINT
       character itself now follows that locale too -- most US/UK-style
       locales use '.', but some (e.g. many European ones) use ',' as the
       decimal point and '.' for grouping instead, the opposite convention.
       If the machine's locale is unset (falls back to "C"/"POSIX"), nothing
       changes from before this call at all. */
    setlocale(LC_NUMERIC, "");
    /* Combine two entropy sources rather than relying on either alone: real
       OS entropy from /dev/urandom (should never repeat), XORed with
       time+PID (so even a misbehaving /dev/urandom read can't produce a
       collision on its own -- both would have to fail identically). */
    unsigned seed = (unsigned)time(NULL) ^ (unsigned)getpid();
    FILE *urandom = fopen("/dev/urandom", "rb");
    if(urandom){
        unsigned urnd;
        if(fread(&urnd, sizeof(urnd), 1, urandom) == 1) seed ^= urnd;
        fclose(urandom);
    }
    srand(seed);
    g_out = stdout;
    loadProgram(argv[1]);
    prescan();
    int pc=0;
    while(pc>=0 && pc<g_nlines){
        int next = execLine(pc);
        pc = (next<0) ? pc+1 : next;
    }
    return 0;
}
