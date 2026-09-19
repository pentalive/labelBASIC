# LabelBASIC

A lightweight BASIC interpreter for learning and scripting. **Simple, expressive, and fun.**

LabelBASIC executes programs line-by-line directly from source—no compilation step. Write code in your favorite text editor (`.lbas` files) and run it immediately. It's an interpreter that brings back the joy of BASIC without the line-number cruft.

## What Can You Do?

- **Calculate and transform data** with clean, readable syntax
- **Process files** line-by-line or interactively
- **Build small scripts** quickly—no boilerplate
- **Learn programming** fundamentals in a forgiving environment
- **Resurrect classic BASIC programs** from vintage books and archives

## Key Features

- **Label-based flow control**: Replace line numbers with meaningful labels. Jump with `GOTO` and call subroutines with `CALL`
- **Loop constructs**: `DO`/`NEXT` loops with optional `WHILE` and `UNTIL` clauses
- **Conditional branching**: `IF`/`THEN`/`ELSE` statements
- **Variables and arrays**: String and numeric variables, plus 1D and 2D arrays
- **Built-in functions**: Math (`sin`, `cos`, `tan`, `rnd`), string operations (`$chr`, `$str`, `$front`, `$back`, `$join`), and more
- **File and terminal I/O**: `OPEN`, `ASK`, `PRINT` with output redirection
- **Terminal graphics**: `XTEND` keyword for ANSI color and cursor control
- **Pre-processor support**: Use `#include` to add function libraries (like math.h)
- **Recent features**: `ELSE` keyword and `nocr()` to suppress newlines on print output

## Quick Example

```basic
; Calculate average of user input
Begin:
    Let sum = 0
    Let num = 0
    Do
        Ask "Enter a number (0 to stop): ", data
        Break if data == 0
        Let sum = sum + data
        Let num = num + 1
    Next
    Print "Average: ", sum / num
    Stop
```

## Running LabelBASIC

```bash
lb your_program.lbas
```

## Documentation

See the included **User Manual** for detailed syntax, all keywords, operators, functions, and examples—including how to convert vintage line-numbered BASIC programs.

## Design Philosophy

LabelBASIC is **deliberately simple**: it focuses on readability and immediate execution. No complex type systems, no module overhead, no build pipeline. Just code and results.

---

*A modern take on a classic language.*