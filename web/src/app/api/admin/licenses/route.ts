import { NextResponse } from 'next/server';
import { z } from 'zod';
import { requireAdmin } from '@/lib/admin';
import { sendLicenseGranted } from '@/lib/email';
import { changeGivenLicense, issueLicense, listGivenLicenses } from '@/lib/license-grants';

export const runtime = 'nodejs';
export const dynamic = 'force-dynamic';

/**
 * Giving licences: academic giveaways and the master licence. Administrators
 * only, refused the way every other admin route refuses.
 */

const forbidden = () =>
  NextResponse.json({ error: 'This area is for administrators.' }, { status: 403 });

const issueSchema = z.object({
  kind: z.enum(['academic', 'master']),
  email: z.string().max(320),
  grantedTo: z.string().max(400),
  seats: z.number(),
  expiresAt: z.string().nullable(),
  /** Whether to email the licence to the recipient. */
  notify: z.boolean().default(true),
});

const changeSchema = z.object({
  licenseId: z.string().uuid(),
  status: z.enum(['active', 'suspended', 'revoked']).optional(),
  expiresAt: z.string().nullable().optional(),
});

export async function GET(): Promise<Response> {
  try {
    await requireAdmin();
  } catch {
    return forbidden();
  }
  return NextResponse.json({ licenses: await listGivenLicenses() });
}

export async function POST(request: Request): Promise<Response> {
  let admin;
  try {
    admin = await requireAdmin();
  } catch {
    return forbidden();
  }

  const parsed = issueSchema.safeParse(await request.json().catch(() => null));
  if (!parsed.success) return NextResponse.json({ error: 'An address and a kind of licence are required' }, { status: 400 });

  const { notify, ...grant } = parsed.data;
  const issued = await issueLicense(grant, admin.userId);
  if (!issued.ok) return NextResponse.json({ error: issued.error }, { status: 400 });

  // The licence exists whether or not the email goes: a failed send is said,
  // and the account page has the serial either way.
  const sent = notify && issued.license.email
    ? await sendLicenseGranted({
        to: issued.license.email,
        userId: issued.userId,
        serial: issued.license.serial,
        kind: grant.kind,
        grantedTo: issued.license.grantedTo,
        seats: issued.license.seats,
        expiresAt: issued.license.expiresAt,
      })
    : null;

  return NextResponse.json({
    license: issued.license,
    accountCreated: issued.accountCreated,
    emailed: sent ? sent.sent : false,
    emailError: sent && !sent.sent ? sent.error : null,
  });
}

export async function PATCH(request: Request): Promise<Response> {
  try {
    await requireAdmin();
  } catch {
    return forbidden();
  }
  const parsed = changeSchema.safeParse(await request.json().catch(() => null));
  if (!parsed.success) return NextResponse.json({ error: 'A licence and a change are required' }, { status: 400 });
  const done = await changeGivenLicense(parsed.data);
  return done.ok ? NextResponse.json({ ok: true }) : NextResponse.json({ error: done.error }, { status: 400 });
}
