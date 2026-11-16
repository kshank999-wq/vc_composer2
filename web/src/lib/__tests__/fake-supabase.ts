import type { SupabaseClient } from '@supabase/supabase-js';

/**
 * A small in-memory stand-in for the Supabase client, covering exactly the
 * calls the fulfillment path makes.
 *
 * It exists because the commerce path is the one place in this site where a
 * bug costs a customer money, and "it is only exercised in production" is not
 * an acceptable answer for it. The unique constraints that make fulfillment
 * idempotent are enforced here too, so a test that passes against this fake is
 * testing the same rule Postgres enforces.
 */

export interface FakeRow {
  [key: string]: unknown;
  id: string;
}

export interface FakeState {
  profiles: FakeRow[];
  orders: FakeRow[];
  licenses: FakeRow[];
  authUsers: Array<{ id: string; email: string }>;
  /** Every insert/upsert attempted, so tests can assert on write counts. */
  writes: Array<{ table: string; operation: 'insert' | 'upsert' | 'update' }>;
}

export class UniqueViolation extends Error {
  constructor(column: string) {
    super(`duplicate key value violates unique constraint on ${column}`);
    this.name = 'UniqueViolation';
  }
}

const clone = <T>(value: T): T => JSON.parse(JSON.stringify(value)) as T;

let sequence = 0;
const nextId = (): string => {
  sequence += 1;
  return `00000000-0000-4000-8000-${String(sequence).padStart(12, '0')}`;
};

interface Filter {
  column: string;
  value: string;
  mode: 'eq' | 'ilike';
}

/** The subset of the query builder fulfillment uses, and nothing more. */
class FakeQuery {
  private filters: Filter[] = [];
  private pending: { operation: 'insert' | 'upsert' | 'update'; row: FakeRow; onConflict?: string } | null = null;
  private failure: Error | null = null;

  constructor(
    private readonly state: FakeState,
    private readonly table: keyof FakeState,
  ) {}

  private get rows(): FakeRow[] {
    return this.state[this.table] as FakeRow[];
  }

  select(): this {
    return this;
  }

  eq(column: string, value: string): this {
    this.filters.push({ column, value, mode: 'eq' });
    return this;
  }

  ilike(column: string, value: string): this {
    this.filters.push({ column, value, mode: 'ilike' });
    return this;
  }

  private matches(row: FakeRow): boolean {
    return this.filters.every((filter) => {
      const actual = row[filter.column];
      if (typeof actual !== 'string') return actual === filter.value;
      return filter.mode === 'ilike'
        ? actual.toLowerCase() === filter.value.toLowerCase()
        : actual === filter.value;
    });
  }

  insert(row: Record<string, unknown>): this {
    this.state.writes.push({ table: this.table, operation: 'insert' });
    // licenses.order_id is unique — the constraint that makes a replayed
    // webhook unable to mint a second license.
    if (this.table === 'licenses' && this.rows.some((existing) => existing['order_id'] === row['order_id'])) {
      this.failure = new UniqueViolation('licenses.order_id');
      return this;
    }
    this.pending = { operation: 'insert', row: { id: nextId(), ...row } as FakeRow };
    return this;
  }

  upsert(row: Record<string, unknown>, options?: { onConflict?: string }): this {
    this.state.writes.push({ table: this.table, operation: 'upsert' });
    const conflictColumn = options?.onConflict;
    if (conflictColumn) {
      const existing = this.rows.find((candidate) => candidate[conflictColumn] === row[conflictColumn]);
      if (existing) {
        Object.assign(existing, row);
        this.pending = { operation: 'upsert', row: existing };
        return this;
      }
    }
    this.pending = { operation: 'upsert', row: { id: nextId(), ...row } as FakeRow };
    return this;
  }

  update(row: Record<string, unknown>): this {
    this.state.writes.push({ table: this.table, operation: 'update' });
    this.pending = { operation: 'update', row: row as FakeRow };
    return this;
  }

  private commit(): FakeRow | null {
    if (!this.pending) return null;
    const { operation, row } = this.pending;

    if (operation === 'update') {
      const targets = this.rows.filter((candidate) => this.matches(candidate));
      for (const target of targets) Object.assign(target, row);
      return targets[0] ?? null;
    }

    if (!this.rows.includes(row)) this.rows.push(row);
    return row;
  }

  async single(): Promise<{ data: FakeRow | null; error: Error | null }> {
    if (this.failure) return { data: null, error: this.failure };
    const committed = this.commit();
    if (committed) return { data: clone(committed), error: null };
    const found = this.rows.filter((row) => this.matches(row));
    return found.length === 1
      ? { data: clone(found[0] as FakeRow), error: null }
      : { data: null, error: new Error('no rows returned') };
  }

  async maybeSingle(): Promise<{ data: FakeRow | null; error: Error | null }> {
    if (this.failure) return { data: null, error: this.failure };
    const committed = this.commit();
    if (committed) return { data: clone(committed), error: null };
    const found = this.rows.find((row) => this.matches(row));
    return { data: found ? clone(found) : null, error: null };
  }

  /** Awaiting the builder without select()/single() resolves the write. */
  then<T>(resolve: (value: { data: FakeRow[] | null; error: Error | null }) => T): T {
    if (this.failure) return resolve({ data: null, error: this.failure });
    const committed = this.commit();
    if (committed) return resolve({ data: [clone(committed)], error: null });
    return resolve({ data: this.rows.filter((row) => this.matches(row)).map(clone), error: null });
  }
}

export interface FakeSupabase {
  client: SupabaseClient;
  state: FakeState;
}

export const createFakeSupabase = (seed: Partial<FakeState> = {}): FakeSupabase => {
  const state: FakeState = {
    profiles: seed.profiles ?? [],
    orders: seed.orders ?? [],
    licenses: seed.licenses ?? [],
    authUsers: seed.authUsers ?? [],
    writes: [],
  };

  const client = {
    from: (table: string) => new FakeQuery(state, table as keyof FakeState),
    auth: {
      admin: {
        createUser: async ({ email }: { email: string }) => {
          const conflict = state.authUsers.find((user) => user.email.toLowerCase() === email.toLowerCase());
          if (conflict) {
            return { data: { user: null }, error: new Error('User already registered') };
          }
          const user = { id: nextId(), email };
          state.authUsers.push(user);
          // The signup trigger mirrors the email onto the profile (migration 0001).
          state.profiles.push({ id: user.id, email, display_name: '' });
          return { data: { user }, error: null };
        },
      },
    },
  } as unknown as SupabaseClient;

  return { client, state };
};
