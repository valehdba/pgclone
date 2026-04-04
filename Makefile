# pgclone Makefile
# Uses PGXS (PostgreSQL Extension Build Infrastructure)

EXTENSION    = pgclone
MODULE_big   = pgclone
OBJS         = src/pgclone.o src/pgclone_bgw.o
DATA         = $(wildcard sql/pgclone--*.sql)

PG_CPPFLAGS  = -I$(shell $(PG_CONFIG) --includedir) -Isrc
SHLIB_LINK   += -lpq

# PostgreSQL Extension build system
PG_CONFIG ?= pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

#old variant, Silersen
#PG_CPPFLAGS  = -I$(libpq_srcdir)
#SHLIB_LINK   = $(libpq)
