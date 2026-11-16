import { NextResponse } from 'next/server';
import { z } from 'zod';
import { describeDevice } from '@/domain';
import { deactivateDevice, listDevices } from '@/lib/activation-service';
import { currentUser } from '@/lib/supabase';

export const runtime = 'nodejs';
export const dynamic = 'force-dynamic';

/** The devices using this account's seats, and the ones that used to. */
export async function GET(): Promise<Response> {
  const user = await currentUser();
  if (!user) return NextResponse.json({ error: 'Sign in to see your devices' }, { status: 401 });

  const devices = await listDevices(user.id);

  return NextResponse.json({
    devices: devices.map(({ activation, serial, maxActivations }) => ({
      id: activation.id,
      label: describeDevice(activation),
      platform: activation.platform,
      appVersion: activation.appVersion,
      activatedAt: activation.activatedAt,
      lastSeenAt: activation.lastSeenAt,
      deactivatedAt: activation.deactivatedAt,
      serial,
      maxActivations,
    })),
  });
}

const deleteSchema = z.object({ activationId: z.string().uuid() });

/**
 * Free a seat (lost-device replacement). The customer does this themselves;
 * the record is kept and marked deactivated rather than deleted, so support
 * can still see the history.
 */
export async function DELETE(request: Request): Promise<Response> {
  const user = await currentUser();
  if (!user) return NextResponse.json({ error: 'Sign in to manage your devices' }, { status: 401 });

  const parsed = deleteSchema.safeParse(await request.json().catch(() => null));
  if (!parsed.success) return NextResponse.json({ error: 'Which device?' }, { status: 400 });

  const result = await deactivateDevice({ userId: user.id, activationId: parsed.data.activationId });
  if (!result.ok) return NextResponse.json({ error: result.error }, { status: 400 });

  return NextResponse.json({ deactivated: true });
}
