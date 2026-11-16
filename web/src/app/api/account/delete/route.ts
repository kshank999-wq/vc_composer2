import { NextResponse } from 'next/server';
import { z } from 'zod';
import { SUPPORT_EMAIL } from '@/lib/site';
import { adminClient, currentUser } from '@/lib/supabase';

export const runtime = 'nodejs';
export const dynamic = 'force-dynamic';

/**
 * Deleting an account.
 *
 * One call: `auth.admin.deleteUser` removes the sign-in, the profile cascades
 * from it, and the licences and their device activations cascade from the
 * profile. **An order survives** as a financial record with nobody attached
 * (`orders.user_id` is set null), because tax law wants the receipt and the
 * receipt does not need a name on it. Both facts are said on the account page
 * before the press.
 *
 * Deleting the account does not cancel a Stripe subscription by itself; the
 * account page tells the customer to cancel in billing first, and support can
 * cancel in Stripe if they did not.
 *
 * The confirmation is the account's **own email address**, typed. A boolean
 * would be a click; this takes a person who knows which account they are
 * signed into and means to end it.
 */
const bodySchema = z.object({
  confirm: z.string().min(1),
});

export async function POST(request: Request): Promise<Response> {
  const user = await currentUser();
  if (!user?.email) {
    return NextResponse.json({ error: 'Sign in to delete your account' }, { status: 401 });
  }

  const parsed = bodySchema.safeParse(await request.json().catch(() => null));
  if (!parsed.success) {
    return NextResponse.json({ error: 'Type your email address to confirm' }, { status: 400 });
  }

  const said = parsed.data.confirm.trim().toLowerCase();
  if (said !== user.email.trim().toLowerCase()) {
    return NextResponse.json(
      { error: `That is not the address on this account. Type ${user.email} to confirm.` },
      { status: 400 },
    );
  }

  const { error } = await adminClient().auth.admin.deleteUser(user.id);
  if (error) {
    return NextResponse.json(
      { error: `Your account could not be deleted. Write to ${SUPPORT_EMAIL} and a person will do it.` },
      { status: 500 },
    );
  }

  return NextResponse.json({ deleted: true });
}
