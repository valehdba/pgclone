# pgclone Makefile
# Uses PGXS (PostgreSQL Extension Build Infrastructure)

EXTENSION    = pgclone
MODULE_big   = pgclone
OBJS         = src/pgclone.o src/pgclone_bgw.o src/pgclone_diff.o src/pgclone_preflight.o src/pgclone_snapshot.o src/pgclone_mask.o src/pgclone_oracle.o src/pgclone_oci.o src/pgclone_sqlserver.o src/pgclone_tds.o
DATA         = $(wildcard sql/pgclone--*.sql)

PG_CPPFLAGS  = -I$(shell $(PG_CONFIG) --includedir) -Isrc
SHLIB_LINK   += -lpq

# Optional Oracle support for pgclone.table_from_oracle() (OCI).
#   make WITH_ORACLE=1 ORACLE_HOME=/opt/oracle/instantclient_23_3
# Needs Oracle Instant Client "Basic" + "SDK" (headers in sdk/include).
ifdef WITH_ORACLE
ORACLE_INC   ?= $(ORACLE_HOME)/sdk/include
ORACLE_LIB   ?= $(ORACLE_HOME)
PG_CPPFLAGS  += -DPGCLONE_WITH_ORACLE -I$(ORACLE_INC)
SHLIB_LINK   += -L$(ORACLE_LIB) -lclntsh
endif

# Optional SQL Server support for pgclone.table_from_sqlserver() (FreeTDS).
#   make WITH_SQLSERVER=1 [FREETDS_HOME=/opt/homebrew/opt/freetds]
# Needs FreeTDS (libsybdb + headers): libsybdb5/freetds-dev on Debian/Ubuntu,
# freetds-devel on RHEL, `brew install freetds` on macOS.
ifdef WITH_SQLSERVER
ifdef FREETDS_HOME
FREETDS_INC  ?= $(FREETDS_HOME)/include
FREETDS_LIB  ?= $(FREETDS_HOME)/lib
PG_CPPFLAGS  += -I$(FREETDS_INC)
SHLIB_LINK   += -L$(FREETDS_LIB)
endif
PG_CPPFLAGS  += -DPGCLONE_WITH_SQLSERVER
SHLIB_LINK   += -lsybdb
endif

# PostgreSQL Extension build system
PG_CONFIG ?= pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

#old variant, Silersen
#PG_CPPFLAGS  = -I$(libpq_srcdir)
#SHLIB_LINK   = $(libpq)
