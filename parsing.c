#include <stdio.h>
#include <stdlib.h>

#include "mpc.h"

/* If we're compiling on Windows compile these functions */
#ifdef _WIN32
#include <string.h>

static char buffer[2048];

char *readline(char *prompt) {
  fputs(prompt, stdout);
  fgets(buffer, 2048, stdin);
  char *cpy = malloc(strlen(buffer) + 1);
  strcpy(cpy, buffer);
  cpy[strlen(cpy) - 1] = '\0';
  return cpy;
}

void add_history(char *unused) {}

/* Otherwise include the editline headers */
#else
#include <editline/readline.h>
#endif

/* lval types */
// NOTE: It is convention to use both enum and typedef enum
// so that we can ommit the enum keyword and be compatibal with c++
enum lval_type { LVAL_NUM, LVAL_ERR, LVAL_SYM, LVAL_SEXPR };
typedef enum lval_type lval_type;

/* error types */
enum { LERR_DIV_ZERO, LERR_BAD_OP, LERR_BAD_NUM };

typedef struct lval {
  lval_type type;
  union { // NOTE: Unions occupy the same memory location
    long num;
    char *err;
    char *sym;
  };
  int count;
  struct lval **cell;
} lval;

/*  Construct a pointer to a new Number lval */
lval *lval_num(long x) {
  lval *v = malloc(sizeof(lval));
  v->type = LVAL_NUM;
  v->num = x;
  return v;
}

/*  Construct a pointer to a new Error lval */
lval *lval_err(char *m) {
  lval *v = malloc(sizeof(lval));
  v->type = LVAL_ERR;
  v->err = malloc(strlen(m) + 1); // Allocates enoug memory to hold the error
  strcpy(v->err, m);              // Copies the error string into memory
  return v;
}

/*  Construct a pointer to a new Symbol lval */
lval *lval_sym(char *s) {
  lval *v = malloc(sizeof(lval));
  v->type = LVAL_SYM;
  v->sym = malloc(strlen(s) + 1);
  strcpy(v->sym, s);
  return v;
}

/*  Construct a pointer to a new empty Sexpr lval */
lval *lval_sexpr(void) {
  lval *v = malloc(sizeof(lval));
  v->type = LVAL_SEXPR;
  v->count = 0;
  v->cell = NULL;
  return v;
}

void lval_del(lval *v) {
  switch (v->type) {
  /*  Do nothing special for the number type */
  case LVAL_NUM:
    break;

    /*  For Err or Sym free the string data */
  case LVAL_ERR:
    free(v->err);
    break;

  case LVAL_SYM:
    free(v->sym);
    break;

  /*  If Sexpr then delete all elements inside */
  case LVAL_SEXPR:
    for (int i = 0; i < v->count; i++) {
      lval_del(v->cell[i]);
    }
    free(v->cell);
    break;
  }

  free(v);
}

/*  Forward declarations  */
void lval_expr_print(lval *v, char open, char close);
lval *lval_eval_sexpr(lval *v);

lval *lval_read_num(mpc_ast_t *t) {
  errno = 0;
  long x = strtol(t->contents, NULL, 10);
  return errno != ERANGE ? lval_num(x) : lval_err("invalid number");
}

lval *lval_add(lval *v, lval *x) {
  v->count++;
  v->cell = realloc(v->cell, sizeof(lval *) * v->count);
  v->cell[v->count - 1] = x;
  return v;
}

lval *lval_read(mpc_ast_t *t) {
  /*  If symbol or number, return conversion to that type */
  if (strstr(t->tag, "number")) {
    return lval_read_num(t);
  }
  if (strstr(t->tag, "symbol")) {
    return lval_sym(t->contents);
  }

  /*  If root (>) or sexpr then create empty list */
  lval *x = NULL;
  if (strcmp(t->tag, ">") == 0) {
    x = lval_sexpr();
  }
  if (strstr(t->tag, "sexpr")) {
    x = lval_sexpr();
  }

  /*  Fill the list with any valid expression contained within */
  for (int i = 0; i < t->children_num; i++) {
    if (strcmp(t->children[i]->contents, "(") == 0) {
      continue;
    }
    if (strcmp(t->children[i]->contents, ")") == 0) {
      continue;
    }
    if (strcmp(t->children[i]->tag, "regex") == 0) {
      continue;
    }
    x = lval_add(x, lval_read(t->children[i]));
  }

  return x;
}

void lval_print(lval *v) {
  switch (v->type) {
  case LVAL_NUM:
    printf("%li", v->num);
    break;
  case LVAL_ERR:
    printf("Error: %s", v->err);
    break;
  case LVAL_SYM:
    printf("%s", v->sym);
    break;
  case LVAL_SEXPR:
    lval_expr_print(v, '(', ')');
    break;
  }
}

void lval_println(lval *v) {
  lval_print(v);
  putchar('\n');
}

void lval_expr_print(lval *v, char open, char close) {
  putchar(open);

  for (int i = 0; i < v->count; i++) {
    /*  Print Value contained within  */
    lval_print(v->cell[i]);

    /*  Dont print trailing space if last element  */
    if (i != (v->count - 1)) {
      putchar(' ');
    }
  }
  putchar(close);
}

lval *lval_pop(lval *v, int i) {
  /*  Find the item at i  */
  lval *x = v->cell[i];

  /*  Decrease the count of items in the list  */
  v->count--;

  /*  Shift memory after the item at "i" over the top  */
  memmove(&v->cell[i], &v->cell[i+1], sizeof(lval*) * (v->count-i));

  /*  Reallocate the memory used  */
  v->cell = realloc(v->cell, sizeof(lval*) * v->count);

  return x;
}

lval *lval_take(lval *v, int i) {
  lval *x = lval_pop(v, i);
  lval_del(v);
  return x;
}

lval *builtin_op(lval *a, char *op) {
  /*  Ensure all arguments are numbers  */
  for (int i = 0; i < a->count; i++) {
    if (a->cell[i]->type != LVAL_NUM) {
      lval_del(a);
      return lval_err("Cannot operator on a non-number!");
    }
  }

  /*  Pop the first element  */
  lval *x = lval_pop(a, 0);

  /*  If no arguments and sub then perform unary negation  */
  if ((strcmp(op, "-") == 0) && a->count == 0) {
    x->num = -x->num;
  }

  /*  While there are still elements remaining  */
  while (a->count > 0) {
    /*  Pop the next element  */
    lval *y = lval_pop(a, 0);

    if(strcmp(op, "+") == 0) {
      x->num += y->num;
    }

    if(strcmp(op, "-") == 0) {
      x->num -= y->num;
    }

    if(strcmp(op, "*") == 0) {
      x->num *= y->num;
    }

    if(strcmp(op, "/") == 0) {
      if(y->num == 0) {
        lval_del(x);
        lval_del(y);

        x = lval_err("Division by zero!");
        break;
      }

      x->num /= y->num;
    }

    lval_del(y);
  }

  lval_del(a);
  return x;
}

lval *lval_eval(lval *v) {
  /*  Evaluate S-expression  */
  if (v->type == LVAL_SEXPR) {
    return lval_eval_sexpr(v);
  }

  return v;
}

lval *lval_eval_sexpr(lval *v) {
  /*  Evaluate children  */
  for (int i = 0; i < v->count; i++) {
    v->cell[i] = lval_eval(v->cell[i]);
  }

  /*  Error checking  */
  for (int i = 0; i < v->count; i++) {
    if (v->cell[i]->type == LVAL_ERR) {
      return lval_take(v, i);
    }
  }

  /*  Empty Expression  */
  if (v->count == 0) {
    return v;
  }

  /*  Single Expression */
  if (v->count == 1) {
    return lval_take(v, 0);
  }

  /*  Ensure first element is symbol  */
  lval *f = lval_pop(v, 0);
  if (f->type != LVAL_SYM) {
    lval_del(f);
    lval_del(v);
    return lval_err("S-expression does not start with symbol!");
  }

  /*  Call builtin with operator  */
  lval *result = builtin_op(v, f->sym);
  lval_del(f);
  return result;
}

int main(int argc, char **argv) {

  /* Create some parsers */
  mpc_parser_t *Number = mpc_new("number");
  mpc_parser_t *Symbol = mpc_new("symbol");
  mpc_parser_t *Sexpr = mpc_new("sexpr");
  mpc_parser_t *Expr = mpc_new("expr");
  mpc_parser_t *Clisp = mpc_new("clisp");

  /* Define parsers with the following language */
  mpca_lang(MPCA_LANG_DEFAULT, "number : /-?[0-9]+/ ; \
symbol : '+' | '-' | '*' | '/' | '%' ; \
sexpr : '(' <expr>* ')' ; \
expr : <number> | <symbol> | <sexpr> ; \
clisp: /^/ <expr>* /$/ ; \
",
            Number, Symbol, Sexpr, Expr, Clisp);

  /*  Print Version and Exit Information */
  puts("CLISP Version 0.0.0.0.1");
  puts("Press Ctrl+c to Exit\n");

  /*  Main loop */
  while (1) {
    /* Now the readline will work correctly on both windows/mac/linux */
    char *input = readline("clisp> ");
    add_history(input);

    /* Attempt to parse the user input */
    mpc_result_t r;
    if (mpc_parse("<stdin>", input, Clisp, &r)) {

      lval *x = lval_eval(lval_read(r.output));
      lval_println(x);
      lval_del(x);
      mpc_ast_delete(r.output);
    } else {
      /*  Otherwise print the error */
      mpc_err_print(r.error);
      mpc_err_delete(r.error);
    }

    /*  Free retrieved input */
    free(input);
  }

  mpc_cleanup(5, Number, Symbol, Sexpr, Expr, Clisp);
  return 0;
}
