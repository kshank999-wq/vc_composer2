import { NextResponse } from 'next/server';
import { z } from 'zod';
import { requireAdmin } from '@/lib/admin';
import { listDiscounts, makeDiscount, switchDiscount } from '@/lib/discounts';

export const runtime = 'nodejs';
export const dynamic = 'force-dynamic';

/**
 * Making and withdrawing discount codes.
 *
 * Administrators only, for the obvious reason, and it refuses the way every
 * other admin route refuses rather than inventing a second wording.
 */

const forbidden = () =>
  NextResponse.json({ error: 'This area is for administrators.' }, { status: 403 });

const planSchema = z.object({
  code: z.string().min(1).max(64),
  off: z.discriminatedUnion('kind', [
    z.object({ kind: z.literal('percent'), percent: z.number() }),
    z.object({ kind: z.literal('amount'), amountCents: z.number(), currency: z.string() }),
  ]),
  maxRedemptions: z.number().int().nullable(),
  expiresAt: z.string().nullable(),
});

const switchSchema = z.object({ promotionCodeId: z.string().min(1), active: z.boolean() });

export async function GET(): Promise<Response> {
  try {
    await requireAdmin();
  } catch {
    return forbidden();
  }
  return NextResponse.json({ offers: await listDiscounts() });
}

export async function POST(request: Request): Promise<Response> {
  try {
    await requireAdmin();
  } catch {
    return forbidden();
  }

  const parsed = planSchema.safeParse(await request.json().catch(() => null));
  if (!parsed.success) {
    return NextResponse.json({ error: 'A code and what it takes off are required' }, { status: 400 });
  }

  // `makeDiscount` asks the domain's refusal again for itself, so this route
  // cannot get past the reading by not reading it.
  const made = await makeDiscount(parsed.data);
  if (!made.ok) return NextResponse.json({ error: made.error }, { status: 400 });
  return NextResponse.json({ offer: made.offer });
}

/** Switching one off; it is never deleted, so what it did can still be read. */
export async function PATCH(request: Request): Promise<Response> {
  try {
    await requireAdmin();
  } catch {
    return forbidden();
  }

  const parsed = switchSchema.safeParse(await request.json().catch(() => null));
  if (!parsed.success) {
    return NextResponse.json({ error: 'A code and a state are required' }, { status: 400 });
  }

  const done = await switchDiscount(parsed.data.promotionCodeId, parsed.data.active);
  if (!done) return NextResponse.json({ error: 'Stripe would not change it.' }, { status: 502 });
  return NextResponse.json({ ok: true });
}
