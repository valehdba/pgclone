-- ============================================================
-- pgclone test fixtures
-- This creates test objects in the source database
-- ============================================================

-- Schema with various object types
CREATE SCHEMA IF NOT EXISTS test_schema;

-- ---- Tables ----
CREATE TABLE test_schema.customers (
    id          SERIAL PRIMARY KEY,
    name        VARCHAR(100) NOT NULL,
    email       VARCHAR(255) UNIQUE,
    status      VARCHAR(20) DEFAULT 'active',
    score       INTEGER CHECK (score >= 0 AND score <= 100),
    created_at  TIMESTAMP DEFAULT now()
);

CREATE TABLE test_schema.orders (
    id          SERIAL PRIMARY KEY,
    customer_id INTEGER NOT NULL REFERENCES test_schema.customers(id),
    total       NUMERIC(10,2) NOT NULL,
    status      VARCHAR(20) DEFAULT 'pending',
    notes       TEXT,
    created_at  TIMESTAMP DEFAULT now()
);

CREATE TABLE test_schema.order_items (
    id          SERIAL PRIMARY KEY,
    order_id    INTEGER NOT NULL REFERENCES test_schema.orders(id),
    product     VARCHAR(200) NOT NULL,
    quantity    INTEGER NOT NULL DEFAULT 1,
    price       NUMERIC(10,2) NOT NULL
);

-- ---- Indexes ----
CREATE INDEX idx_customers_status ON test_schema.customers(status);
CREATE INDEX idx_customers_name_lower ON test_schema.customers(lower(name));
CREATE INDEX idx_orders_customer ON test_schema.orders(customer_id);
CREATE INDEX idx_orders_created ON test_schema.orders(created_at DESC);
CREATE INDEX idx_order_items_order ON test_schema.order_items(order_id);

-- ---- Exclusion constraint (requires btree_gist) ----
-- CREATE EXTENSION IF NOT EXISTS btree_gist;
-- ALTER TABLE test_schema.orders ADD CONSTRAINT excl_no_overlap
--     EXCLUDE USING gist (customer_id WITH =, tsrange(created_at, created_at + interval '1 hour') WITH &&);

-- ---- Trigger function ----
CREATE OR REPLACE FUNCTION test_schema.update_timestamp()
RETURNS TRIGGER AS $$
BEGIN
    NEW.created_at = now();
    RETURN NEW;
END;
$$ LANGUAGE plpgsql;

CREATE OR REPLACE FUNCTION test_schema.log_order_change()
RETURNS TRIGGER AS $$
BEGIN
    RAISE NOTICE 'Order % changed: status = %', NEW.id, NEW.status;
    RETURN NEW;
END;
$$ LANGUAGE plpgsql;

-- ---- Triggers ----
CREATE TRIGGER trg_orders_timestamp
    BEFORE UPDATE ON test_schema.orders
    FOR EACH ROW
    EXECUTE FUNCTION test_schema.update_timestamp();

CREATE TRIGGER trg_orders_log
    AFTER UPDATE OF status ON test_schema.orders
    FOR EACH ROW
    EXECUTE FUNCTION test_schema.log_order_change();

-- ---- Sequences ----
CREATE SEQUENCE test_schema.invoice_seq START WITH 1000 INCREMENT BY 1;

-- ---- Views ----
CREATE VIEW test_schema.active_customers AS
    SELECT id, name, email FROM test_schema.customers WHERE status = 'active';

CREATE VIEW test_schema.order_summary AS
    SELECT c.name, COUNT(o.id) AS order_count, SUM(o.total) AS total_spent
    FROM test_schema.customers c
    LEFT JOIN test_schema.orders o ON o.customer_id = c.id
    GROUP BY c.name;

-- ---- Materialized View ----
CREATE MATERIALIZED VIEW test_schema.customer_stats AS
    SELECT
        c.id,
        c.name,
        COUNT(o.id) AS order_count,
        COALESCE(SUM(o.total), 0) AS total_spent,
        MAX(o.created_at) AS last_order
    FROM test_schema.customers c
    LEFT JOIN test_schema.orders o ON o.customer_id = c.id
    GROUP BY c.id, c.name;

CREATE UNIQUE INDEX idx_customer_stats_id ON test_schema.customer_stats(id);

-- ---- Utility functions ----
CREATE OR REPLACE FUNCTION test_schema.get_customer_orders(p_customer_id INTEGER)
RETURNS TABLE(order_id INTEGER, total NUMERIC, status VARCHAR) AS $$
BEGIN
    RETURN QUERY
    SELECT o.id, o.total, o.status
    FROM test_schema.orders o
    WHERE o.customer_id = p_customer_id
    ORDER BY o.created_at DESC;
END;
$$ LANGUAGE plpgsql;

-- ---- Seed data ----
INSERT INTO test_schema.customers (name, email, status, score) VALUES
    ('Alice Johnson', 'alice@example.com', 'active', 95),
    ('Bob Smith', 'bob@example.com', 'active', 82),
    ('Charlie Brown', 'charlie@example.com', 'inactive', 45),
    ('Diana Prince', 'diana@example.com', 'active', 99),
    ('Eve Wilson', 'eve@example.com', 'suspended', 30),
    ('Frank Castle', 'frank@example.com', 'active', 78),
    ('Grace Hopper', 'grace@example.com', 'active', 100),
    ('Henry Ford', 'henry@example.com', 'inactive', 55),
    ('Ivy Chen', 'ivy@example.com', 'active', 88),
    ('Jack Ryan', 'jack@example.com', 'active', 72);

INSERT INTO test_schema.orders (customer_id, total, status, notes) VALUES
    (1, 150.00, 'completed', 'First order'),
    (1, 89.50, 'completed', NULL),
    (2, 245.00, 'completed', 'Bulk order'),
    (2, 67.30, 'pending', NULL),
    (4, 1200.00, 'completed', 'Premium order'),
    (4, 450.00, 'shipped', NULL),
    (6, 33.99, 'pending', 'Small order'),
    (7, 999.00, 'completed', NULL),
    (9, 178.50, 'completed', NULL),
    (10, 56.00, 'cancelled', 'Customer cancelled');

INSERT INTO test_schema.order_items (order_id, product, quantity, price) VALUES
    (1, 'Widget A', 3, 25.00),
    (1, 'Widget B', 1, 75.00),
    (2, 'Gadget X', 2, 44.75),
    (3, 'Mega Pack', 5, 49.00),
    (5, 'Premium Suite', 1, 1200.00),
    (6, 'Widget A', 10, 25.00),
    (6, 'Gadget X', 4, 50.00),
    (8, 'Enterprise License', 1, 999.00),
    (9, 'Widget B', 3, 59.50),
    (10, 'Widget A', 2, 28.00);

-- Refresh materialized view with data
REFRESH MATERIALIZED VIEW test_schema.customer_stats;

-- ---- Simple public schema table for basic tests ----
CREATE TABLE public.simple_test (
    id    SERIAL PRIMARY KEY,
    value TEXT NOT NULL,
    num   INTEGER DEFAULT 0
);

INSERT INTO public.simple_test (value, num) VALUES
    ('hello', 1), ('world', 2), ('foo', 3), ('bar', 4), ('baz', 5);

-- ---- Table with sensitive data for masking tests ----
CREATE TABLE test_schema.employees (
    id          SERIAL PRIMARY KEY,
    full_name   VARCHAR(100) NOT NULL,
    email       VARCHAR(255) NOT NULL,
    phone       VARCHAR(20),
    salary      INTEGER NOT NULL,
    ssn         VARCHAR(11),
    notes       TEXT,
    created_at  TIMESTAMP DEFAULT now()
);

INSERT INTO test_schema.employees (full_name, email, phone, salary, ssn, notes) VALUES
    ('Alice Johnson',  'alice@example.com',    '+1-555-123-4567', 95000, '123-45-6789', 'Senior engineer'),
    ('Bob Smith',      'bob@company.org',      '+1-555-987-6543', 82000, '234-56-7890', 'Team lead'),
    ('Charlie Brown',  'charlie@example.com',  '+1-555-111-2222', 67000, '345-67-8901', NULL),
    ('Diana Prince',   'diana@wonder.net',     '+1-555-333-4444', 120000, '456-78-9012', 'Director'),
    ('Eve Wilson',     'eve@example.com',      NULL,              55000, '567-89-0123', 'Intern');
