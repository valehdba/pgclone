ARG PG_VERSION=18
FROM postgres:${PG_VERSION}

# Install build dependencies
RUN apt-get update && apt-get install -y \
    build-essential \
    postgresql-server-dev-${PG_MAJOR} \
    libpq-dev \
    git \
    && rm -rf /var/lib/apt/lists/*

# Install pgTAP
RUN git clone --depth 1 https://github.com/theory/pgtap.git /tmp/pgtap && \
    cd /tmp/pgtap && \
    make && make install && \
    rm -rf /tmp/pgtap

# Copy extension source
COPY . /build/pgclone
WORKDIR /build/pgclone

# Build and install
RUN make clean && make && make install

# Configure shared_preload_libraries — append to postgresql.conf.sample
# AND create an early init script that also sets it at runtime
# (different PG versions may use different sample file locations)
RUN for f in /usr/share/postgresql/postgresql.conf.sample \
             /usr/share/postgresql/*/postgresql.conf.sample; do \
        if [ -f "$f" ]; then \
            echo "shared_preload_libraries = 'pgclone'" >> "$f"; \
            echo "max_worker_processes = 32" >> "$f"; \
        fi; \
    done

# Early init script: ensure shared_preload_libraries is set before tests run
# This runs after initdb but before the test script
RUN echo '#!/bin/bash' > /docker-entrypoint-initdb.d/00-configure-pgclone.sh && \
    echo 'echo "shared_preload_libraries = '\''pgclone'\''" >> "$PGDATA/postgresql.conf"' >> /docker-entrypoint-initdb.d/00-configure-pgclone.sh && \
    echo 'echo "max_worker_processes = 32" >> "$PGDATA/postgresql.conf"' >> /docker-entrypoint-initdb.d/00-configure-pgclone.sh && \
    echo '# Restart to pick up shared_preload_libraries' >> /docker-entrypoint-initdb.d/00-configure-pgclone.sh && \
    echo 'pg_ctl -D "$PGDATA" -m fast -w restart' >> /docker-entrypoint-initdb.d/00-configure-pgclone.sh && \
    chmod +x /docker-entrypoint-initdb.d/00-configure-pgclone.sh

# Copy test runner scripts
COPY test/run_tests.sh /docker-entrypoint-initdb.d/99-run-tests.sh
RUN chmod +x /docker-entrypoint-initdb.d/99-run-tests.sh
