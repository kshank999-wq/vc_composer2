'use client';

import { useCallback, useEffect, useState } from 'react';

interface DeviceRow {
  id: string;
  label: string;
  appVersion: string;
  activatedAt: string;
  lastSeenAt: string | null;
  deactivatedAt: string | null;
  serial: string;
  /** Null on a licence that covers any number of computers. */
  maxActivations: number | null;
}

const formatDate = (value: string | null): string =>
  value ? new Date(value).toLocaleDateString(undefined, { year: 'numeric', month: 'short', day: 'numeric' }) : '—';

/**
 * Device management.
 *
 * A customer whose laptop was lost or replaced frees the seat here rather than
 * emailing support.
 */
export function Devices() {
  const [devices, setDevices] = useState<DeviceRow[]>([]);
  const [loading, setLoading] = useState(true);
  const [busy, setBusy] = useState<string | null>(null);
  const [error, setError] = useState<string | null>(null);

  const load = useCallback(async () => {
    const response = await fetch('/api/licenses/devices');
    setLoading(false);
    if (!response.ok) return;
    const payload = (await response.json()) as { devices?: DeviceRow[] };
    setDevices(payload.devices ?? []);
  }, []);

  useEffect(() => {
    void load();
  }, [load]);

  const deactivate = async (device: DeviceRow) => {
    setBusy(device.id);
    setError(null);
    const response = await fetch('/api/licenses/devices', {
      method: 'DELETE',
      headers: { 'content-type': 'application/json' },
      body: JSON.stringify({ activationId: device.id }),
    });
    setBusy(null);
    if (!response.ok) {
      const payload = (await response.json()) as { error?: string };
      setError(payload.error ?? 'That device could not be freed');
      return;
    }
    await load();
  };

  if (loading) return <p className="lede">Loading your devices…</p>;

  const active = devices.filter((device) => device.deactivatedAt === null);
  const past = devices.filter((device) => device.deactivatedAt !== null);
  // Any licence covering any number of computers makes the account's limit none.
  const limit = devices.some((device) => device.maxActivations === null)
    ? null
    : (devices[0]?.maxActivations ?? 0);

  if (devices.length === 0) {
    return (
      <p className="lede">
        No computers yet. VC Composer activates the first time you sign in to it on a computer.
      </p>
    );
  }

  return (
    <>
      <p className="lede">
        {limit === null
          ? `${active.length} ${active.length === 1 ? 'computer' : 'computers'} activated. Your licence covers any number, so there is no seat to free — remove one here only to tidy the list.`
          : `${active.length} of ${limit} ${limit === 1 ? 'seat' : 'seats'} in use. Free one here if you have replaced a computer — no need to contact support.`}
      </p>

      <ul className="order-list">
        {active.map((device) => (
          <li key={device.id} className="card">
            <div className="build-row">
              <div>
                <strong>{device.label}</strong>
                <p className="lede">
                  Activated {formatDate(device.activatedAt)}
                  {device.appVersion ? ` · version ${device.appVersion}` : ''}
                  {device.lastSeenAt ? ` · last seen ${formatDate(device.lastSeenAt)}` : ''}
                </p>
              </div>
              <button
                type="button"
                className="button secondary"
                disabled={busy !== null}
                onClick={() => void deactivate(device)}
              >
                {busy === device.id ? 'Freeing…' : 'Free this seat'}
              </button>
            </div>
          </li>
        ))}
      </ul>

      {past.length > 0 ? (
        <p className="lede" style={{ marginTop: 16 }}>
          {past.length} previously activated {past.length === 1 ? 'device' : 'devices'} on record. Signing in on
          one of them again reclaims its seat if one is free.
        </p>
      ) : null}

      {error ? (
        <p className="error" role="alert">
          {error}
        </p>
      ) : null}
    </>
  );
}
