# LabelBasic Preprocessor Wrapper

`lb` is a shell script wrapper that preprocesses LabelBasic source files through the C preprocessor (`cpp`) before executing them. This allows you to use C preprocessor directives like `#include`, `#define`, and other macros in LabelBasic programs.

## Installation

1. Copy `lb` to a directory in your `$PATH` (e.g., `/usr/local/bin/`):
   ```bash
   sudo cp lb /usr/local/bin/
   sudo chmod +x /usr/local/bin/lb
   ```

2. Copy `math.h` to your LabelBasic project directory (or `/usr/local/include/`):
   ```bash
   cp math.h ~/project/labelBasic/
   ```

## Usage

```bash
lb program.lbas [arguments...]
```

The `lb` script:
1. Preprocesses `program.lbas` through `cpp`
2. Creates a temporary preprocessed file
3. Executes it with `labelbasic`
4. Cleans up the temporary file

## Example

Create `usecpp.lbas`:
```basic
#include "math.h"

BEGIN:
  ; Calculate square root
  LET x = sqrt(16)
  PRINT "sqrt(16) = " x
  STOP
END
```

Run it:
```bash
lb usecpp.lbas
```

Output:
```
sqrt(16) = 4
```

## Comments in Preprocessed Files

Use semicolon (`;`) for comments in files that will be preprocessed:

```basic
#include "math.h"

BEGIN:
  ; This is a comment
  LET x = sqrt(25)   ; semicolon works here too
  PRINT x
  STOP
END
```

The `#` character is reserved for preprocessor directives (`#include`, `#define`, etc.).

## Macro Files

### math.h

Currently includes:
- `sqrt(x)` — computes square root as `x^(1/2)`

Add more mathematical macros as needed:
```c
#define abs(x) ((x) < 0 ? -(x) : (x))
#define min(a,b) ((a) < (b) ? (a) : (b))
#define max(a,b) ((a) > (b) ? (a) : (b))
```

## How It Works

The C preprocessor expands macros textually before LabelBasic sees the code:

**Before preprocessing (`usecpp.lbas`):**
```basic
#include "math.h"
PRINT sqrt(9)
```

**After preprocessing (temporary file):**
```basic
PRINT (9^(1/2))
```

**Then executed by labelbasic.**

## Notes

- The preprocessor removes line directives to keep output clean
- Include paths search: current directory, source directory, `/usr/local/include/`
- Temporary files are created in `/tmp/` and cleaned up automatically
- Use semicolons (`;`) for all comments in preprocessed files
- The `#` character is reserved for preprocessor directives only
- A LabelBASIC program that will make use of the preprocessor will not have a hashbang.


