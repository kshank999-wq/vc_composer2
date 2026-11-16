'use client';

import { useState } from 'react';
import {
  ACADEMIC_MAX_SEATS,
  ACADEMIC_SEATS,
  academicDefaultEnd,
  licenseGrantRefusal,
  licenseKindName,
  type GivenLicense,
  type LicenseGrantRequest,
} from '@/domain';
import type { GivenLicenseRow } from '@/lib/license-grants';

/**
 * Giving a licence, and the list of those given. The refusal
 * under the form is the domain's, and the route asks it again.
 */

const formatDate = (value: string | null): string =>
  value ? new Date(value).toLocaleDateString(undefined, { year: 'numeric', month: 'short', day: 'numeric', timeZone: 'UTC' }) : 'Never';

const endOfDay = (day: string): string | null => (day ? new Date(`${day}T23:59:59Z`).toISOString() : null);

export function LicenseGiver({
  initialLicenses,
  adminEmail,
}: {
  initialLicenses: GivenLicenseRow[];
  adminEmail: string | null;
}) {
  const [licenses, setLicenses] = useState(initialLicenses);
  const [kind, setKind] = useState<GivenLicense>('academic');
  const [email, setEmail] = useState('');
  const [grantedTo, setGrantedTo] = useState('');
  const [seats, setSeats] = useState(String(ACADEMIC_SEATS));
  const [ends, setEnds] = useState(academicDefaultEnd(new Date()).slice(0, 10));
  const [notify, setNotify] = useState(true);
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const [done, setDone] = useState<string | null>(null);

  const plan: LicenseGrantRequest = {
    kind,
    email,
    grantedTo,
    seats: Number(seats),
    expiresAt: kind === 'master' ? null : endOfDay(ends),
  };
  const refusal = licenseGrantRefusal(plan, new Date());

  const issue = async () => {
    setBusy(true);
    setError(null);
    setDone(null);
    try {
      const response = await fetch('/api/admin/licenses', {
        method: 'POST',
        headers: { 'content-type': 'application/json' },
        body: JSON.stringify({ ...plan, notify }),
      });
      const payload = (await response.json()) as {
        license?: GivenLicenseRow;
        accountCreated?: boolean;
        emailed?: boolean;
        emailError?: string | null;
        error?: string;
      };
      if (!response.ok || !payload.license) throw new Error(payload.error ?? 'The license could not be issued.');
      setLicenses((was) => [payload.license as GivenLicenseRow, ...was]);
      setDone(
        [
          `${licenseKindName(payload.license.grant)} ${payload.license.serial} issued to ${payload.license.email}.`,
          payload.accountCreated ? 'An account was made for that address.' : null,
          notify ? (payload.emailed ? 'The license was emailed.' : `The email did not go${payload.emailError ? ` (${payload.emailError})` : ''} — the serial is on their account page.`) : null,
        ]
          .filter(Boolean)
          .join(' '),
      );
      setEmail('');
      setGrantedTo('');
    } catch (cause) {
      setError(cause instanceof Error ? cause.message : 'The license could not be issued.');
    } finally {
      setBusy(false);
    }
  };

  const change = async (license: GivenLicenseRow, body: { status?: string; expiresAt?: string | null }) => {
    setError(null);
    const response = await fetch('/api/admin/licenses', {
      method: 'PATCH',
      headers: { 'content-type': 'application/json' },
      body: JSON.stringify({ licenseId: license.id, ...body }),
    });
    if (!response.ok) {
      const payload = (await response.json().catch(() => ({}))) as { error?: string };
      setError(payload.error ?? 'The license could not be changed.');
      return;
    }
    setLicenses((was) =>
      was.map((one) =>
        one.id === license.id
          ? {
              ...one,
              ...(body.status ? { status: body.status as GivenLicenseRow['status'] } : {}),
              ...(body.expiresAt !== undefined ? { expiresAt: body.expiresAt } : {}),
            }
          : one,
      ),
    );
  };

  const extend = (license: GivenLicenseRow) => {
    const from = license.expiresAt && new Date(license.expiresAt) > new Date() ? new Date(license.expiresAt) : new Date();
    void change(license, { expiresAt: academicDefaultEnd(from) });
  };

  return (
    <>
      <section className="card">
        <h2>Give a license</h2>
        <div className="discount-fields">
          <label className="field">
            <span>Kind</span>
            <select value={kind} onChange={(event) => setKind(event.target.value as GivenLicense)}>
              <option value="academic">Academic</option>
              <option value="master">Master — any number of computers</option>
            </select>
          </label>

          <label className="field">
            <span>Email</span>
            <input
              type="email"
              value={email}
              onChange={(event) => setEmail(event.target.value)}
              placeholder={kind === 'master' ? (adminEmail ?? 'you@example.com') : 'teacher@school.edu'}
              autoComplete="off"
            />
          </label>

          <label className="field">
            <span>{kind === 'master' ? 'Note' : 'For'}</span>
            <input
              value={grantedTo}
              onChange={(event) => setGrantedTo(event.target.value)}
              placeholder={kind === 'master' ? 'Optional' : 'School and course, or the student'}
            />
          </label>

          {kind === 'academic' ? (
            <>
              <label className="field">
                <span>Computers</span>
                <input
                  type="number"
                  min={1}
                  max={ACADEMIC_MAX_SEATS}
                  value={seats}
                  onChange={(event) => setSeats(event.target.value)}
                />
              </label>
              <label className="field">
                <span>Ends</span>
                <input type="date" value={ends} onChange={(event) => setEnds(event.target.value)} />
              </label>
            </>
          ) : null}
        </div>

        <label className="field-note" style={{ display: 'flex', gap: 8, alignItems: 'center' }}>
          <input type="checkbox" checked={notify} onChange={(event) => setNotify(event.target.checked)} />
          Email the license to that address
        </label>

        <p className="field-note">
          {refusal ??
            (kind === 'master'
              ? 'A master license covers any number of computers and never ends.'
              : `Covers ${Number(seats) === 1 ? '1 computer' : `${seats} computers`}${ends ? ` until ${formatDate(endOfDay(ends))}` : ', with no end date'}. Leave the date empty for one that does not end.`)}
        </p>

        <button type="button" className="button" onClick={issue} disabled={busy || refusal !== null}>
          {busy ? 'Issuing…' : 'Issue the license'}
        </button>

        {done ? <p className="field-note" role="status">{done}</p> : null}
        {error ? (
          <p className="error" role="alert">
            {error}
          </p>
        ) : null}
      </section>

      <section className="card">
        <h2>Given licenses</h2>
        {licenses.length === 0 ? (
          <p className="lede">None yet.</p>
        ) : (
          <div className="data-table">
            <table>
              <thead>
                <tr>
                  <th>Kind</th>
                  <th>Serial</th>
                  <th>Account</th>
                  <th>For</th>
                  <th>Computers</th>
                  <th>Ends</th>
                  <th>Status</th>
                  <th />
                </tr>
              </thead>
              <tbody>
                {licenses.map((license) => (
                  <tr key={license.id} className={license.status === 'active' ? undefined : 'spent'}>
                    <td>{licenseKindName(license.grant)}</td>
                    <td>
                      <code>{license.serial}</code>
                    </td>
                    <td>{license.email ?? '—'}</td>
                    <td className="wrap">{license.grantedTo || '—'}</td>
                    <td>
                      {license.inUse} in use · {license.seats}
                    </td>
                    <td>{formatDate(license.expiresAt)}</td>
                    <td>{license.status}</td>
                    <td>
                      {license.status === 'active' ? (
                        <button type="button" className="text-act" onClick={() => void change(license, { status: 'suspended' })}>
                          Suspend
                        </button>
                      ) : (
                        <button type="button" className="text-act" onClick={() => void change(license, { status: 'active' })}>
                          Restore
                        </button>
                      )}
                      {license.grant === 'academic' ? (
                        <button type="button" className="text-act" onClick={() => extend(license)}>
                          Extend a year
                        </button>
                      ) : null}
                    </td>
                  </tr>
                ))}
              </tbody>
            </table>
          </div>
        )}
      </section>
    </>
  );
}
