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

# Copy test runner script
COPY test/run_tests.sh /docker-entrypoint-initdb.d/99-run-tests.sh
RUN chmod +x /docker-entrypoint-initdb.d/99-run-tests.sh
