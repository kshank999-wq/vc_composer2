import { inviteSchema, roleWords } from '@/domain';
import { badRequest, collabRoute } from '@/lib/collab-route';
import { invite } from '@/lib/collab-service';
import { sendCollabInvite } from '@/lib/email';
import { RULES } from '@/lib/rate-limit';
import { adminClient } from '@/lib/supabase';

export const runtime = 'nodejs';
export const dynamic = 'force-dynamic';

/**
 * Invite someone by email (the owner only). Editors and arrangers take a
 * seat on the owner's plan; reviewers and viewers join free.
 */
export const POST = (request: Request, { params }: { params: { id: string } }) =>
  collabRoute(
    request,
    async (db, userId) => {
      const parsed = inviteSchema.safeParse(await request.json().catch(() => null));
      if (!parsed.success) return badRequest('Give an email address and a role');
      const made = await invite(db, params.id, userId, parsed.data.email, parsed.data.role);
      if (!made.ok) return made;
      const { data: me } = await adminClient().from('profiles').select('display_name,email').eq('id', userId).maybeSingle();
      const sent = await sendCollabInvite({
        to: parsed.data.email,
        token: made.value.token,
        projectName: made.value.projectName,
        inviterName: me?.display_name || me?.email || '',
        roleLabel: roleWords(parsed.data.role).label,
      });
      return { ok: true, value: { invited: true, emailed: sent.sent } };
    },
    RULES.collabInvite,
  );
