# pgx_clone Makefile
# Uses PGXS (PostgreSQL Extension Build Infrastructure)

EXTENSION    = pgx_clone
MODULE_big   = pgx_clone
OBJS         = src/pgx_clone.o
DATA         = sql/pgx_clone--0.1.0.sql

PG_CPPFLAGS  = -I$(shell $(PG_CONFIG) --includedir)
SHLIB_LINK   += -lpq

# PostgreSQL Extension build system
PG_CONFIG ?= pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

#old variant, remove then
#PG_CPPFLAGS  = -I$(libpq_srcdir)
#SHLIB_LINK   = $(libpq)
