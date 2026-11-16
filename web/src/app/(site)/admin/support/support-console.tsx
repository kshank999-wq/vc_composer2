'use client';

import { useEffect, useState } from 'react';

interface LicenseRow {
  id: string;
  serial: string;
  status: string;
  entitled_platforms: string[];
  max_activations: number;
  /** Addendum 44: how it came to be. Absent before migration 0069. */
  grant_kind?: string;
  granted_to?: string;
  created_at: string;
}

interface OrderRow {
  id: string;
  status: string;
  amount_cents: number;
  currency: string;
  selected_platform: string | null;
  paid_at: string | null;
  created_at: string;
}

interface DeviceRow {
  id: string;
  licenseId: string;
  label: string;
  activatedAt: string;
  deactivatedAt: string | null;
}

interface EmailRow {
  template: string;
  status: string;
  error: string | null;
  created_at: string;
}

interface Lookup {
  found: boolean;
  customer?: { id: string; email: string; created_at: string };
  licenses?: LicenseRow[];
  orders?: OrderRow[];
  devices?: DeviceRow[];
  emails?: EmailRow[];
}

const formatDate = (value: string | null): string =>
  value ? new Date(value).toLocaleString() : '—';

/**
 * `initialEmail` comes from the customers list, so a row there opens the
 * record without a second search.
 */
export function SupportConsole({ initialEmail = '' }: { initialEmail?: string }) {
  const [email, setEmail] = useState(initialEmail);
  const [result, setResult] = useState<Lookup | null>(null);
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const [message, setMessage] = useState<string | null>(null);

  const lookup = async (event?: React.FormEvent) => {
    event?.preventDefault();
    setBusy(true);
    setError(null);
    setMessage(null);
    const response = await fetch(`/api/admin/support?email=${encodeURIComponent(email.trim())}`);
    setBusy(false);
    if (!response.ok) {
      const payload = (await response.json()) as { error?: string };
      setError(payload.error ?? 'That lookup failed');
      return;
    }
    setResult((await response.json()) as Lookup);
  };

  // Arriving with an address means "show me this customer", not "here is a
  // form". Once, on mount; typing afterwards is the user's business.
  useEffect(() => {
    if (initialEmail.trim()) void lookup();
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);

  const act = async (body: Record<string, unknown>, done: string) => {
    setBusy(true);
    setError(null);
    setMessage(null);
    const response = await fetch('/api/admin/support', {
      method: 'POST',
      headers: { 'content-type': 'application/json' },
      body: JSON.stringify(body),
    });
    setBusy(false);
    if (!response.ok) {
      const payload = (await response.json()) as { error?: string };
      setError(payload.error ?? 'That action failed');
      return;
    }
    setMessage(done);
    await lookup();
  };

  return (
    <>
      <section>
        <form onSubmit={lookup} className="inline-form" style={{ display: 'flex', gap: 12 }}>
          <input
            type="email"
            value={email}
            onChange={(event) => setEmail(event.target.value)}
            placeholder="customer@example.com"
            aria-label="Customer email"
            required
          />
          <button type="submit" className="button" disabled={busy}>
            {busy ? 'Looking…' : 'Look up'}
          </button>
        </form>
        {error ? (
          <p className="error" role="alert">
            {error}
          </p>
        ) : null}
        {message ? <p className="notice">{message}</p> : null}
      </section>

      {result && !result.found ? (
        <section>
          <p className="lede">No account with that address. They may have bought under a different one.</p>
        </section>
      ) : null}

      {result?.found && result.customer ? (
        <>
          <section>
            <h2>Licenses</h2>
            {(result.licenses ?? []).length === 0 ? (
              <p className="lede">No licenses. Check their purchases below — fulfillment may have failed.</p>
            ) : (
              <ul className="order-list">
                {(result.licenses ?? []).map((license) => (
                  <li key={license.id} className="card">
                    <div className="build-row">
                      <div>
                        <strong className="serial">{license.serial}</strong>
                        <p className="lede">
                          {license.grant_kind === 'master' ? 'master · ' : license.grant_kind === 'academic' ? 'academic · ' : ''}
                          {license.status} · {license.entitled_platforms.join(', ')} ·{' '}
                          {license.grant_kind === 'master' ? 'any number of computers' : `up to ${license.max_activations} devices`}
                          {license.granted_to ? ` · for ${license.granted_to}` : ''}
                        </p>
                      </div>
                      <div style={{ display: 'flex', gap: 8, flexWrap: 'wrap' }}>
                        <button
                          type="button"
                          className="button secondary"
                          disabled={busy}
                          onClick={() =>
                            void act({ action: 'resend_license', licenseId: license.id }, 'License email sent.')
                          }
                        >
                          Resend email
                        </button>
                        {license.status === 'active' ? (
                          <button
                            type="button"
                            className="button secondary"
                            disabled={busy}
                            onClick={() =>
                              void act(
                                { action: 'set_license_status', licenseId: license.id, status: 'revoked' },
                                'License revoked.',
                              )
                            }
                          >
                            Revoke
                          </button>
                        ) : (
                          <button
                            type="button"
                            className="button secondary"
                            disabled={busy}
                            onClick={() =>
                              void act(
                                { action: 'set_license_status', licenseId: license.id, status: 'active' },
                                'License restored.',
                              )
                            }
                          >
                            Restore
                          </button>
                        )}
                      </div>
                    </div>
                  </li>
                ))}
              </ul>
            )}
          </section>

          <section>
            <h2>Devices</h2>
            {(result.devices ?? []).length === 0 ? (
              <p className="lede">No devices activated.</p>
            ) : (
              <ul className="order-list">
                {(result.devices ?? []).map((device) => (
                  <li key={device.id} className="card">
                    <div className="build-row">
                      <div>
                        <strong>{device.label}</strong>
                        <p className="lede">
                          Activated {formatDate(device.activatedAt)}
                          {device.deactivatedAt ? ` · freed ${formatDate(device.deactivatedAt)}` : ''}
                        </p>
                      </div>
                      {device.deactivatedAt ? null : (
                        <button
                          type="button"
                          className="button secondary"
                          disabled={busy}
                          onClick={() => void act({ action: 'free_device', activationId: device.id }, 'Seat freed.')}
                        >
                          Free seat
                        </button>
                      )}
                    </div>
                  </li>
                ))}
              </ul>
            )}
          </section>

          <section>
            <h2>Purchases</h2>
            {(result.orders ?? []).length === 0 ? (
              <p className="lede">No orders recorded.</p>
            ) : (
              <ul className="order-list">
                {(result.orders ?? []).map((order) => (
                  <li key={order.id} className="card">
                    <strong>
                      {(order.amount_cents / 100).toFixed(2)} {order.currency.toUpperCase()}
                    </strong>
                    <p className="lede">
                      {order.status} · {formatDate(order.paid_at ?? order.created_at)}
                      {order.selected_platform ? ` · chose ${order.selected_platform}` : ''}
                    </p>
                  </li>
                ))}
              </ul>
            )}
          </section>

          <section>
            <h2>Email</h2>
            {(result.emails ?? []).length === 0 ? (
              <p className="lede">Nothing sent yet.</p>
            ) : (
              <ul className="order-list">
                {(result.emails ?? []).map((entry, index) => (
                  <li key={`${entry.created_at}-${index}`} className="card">
                    <strong>{entry.template}</strong>
                    <p className="lede">
                      {entry.status} · {formatDate(entry.created_at)}
                      {entry.error ? ` · ${entry.error}` : ''}
                    </p>
                  </li>
                ))}
              </ul>
            )}
          </section>
        </>
      ) : null}
    </>
  );
}
