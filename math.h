; LabelBasic Math Library
; Useful mathematical macros for use with the lb preprocessor
; Note: These macros use only basic arithmetic and comparison operators

; Power and root functions
#define sqrt(x) ((x)^(1/2))
#define cbrt(x) ((x)^(1/3))

; Common powers
#define sq(x) ((x)*(x))
#define cube(x) ((x)*(x)*(x))

; Trigonometric conversions
#define deg2rad(d) ((d) * 3.14159265359 / 180)
#define rad2deg(r) ((r) * 180 / 3.14159265359)

; Conditional functions (using built-in ternary function)
#define abs(x) ternary(x >= 0, x, -x)
#define min(a,b) ternary(a < b, a, b)
#define max(a,b) ternary(a > b, a, b)
#define clamp(x,lo,hi) ternary(x < lo, lo, ternary(x > hi, hi, x))

; Mathematical constants
#define PI 3.14159265359
#define E 2.71828182846
#define PHI 1.61803398875


