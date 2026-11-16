import { collabRoute } from '@/lib/collab-route';
import { removeMember } from '@/lib/collab-service';

export const runtime = 'nodejs';
export const dynamic = 'force-dynamic';

/** Remove someone from a project (the owner only); their seat frees up. */
export const DELETE = (request: Request, { params }: { params: { id: string; memberId: string } }) =>
  collabRoute(request, (db, userId) => removeMember(db, params.id, userId, params.memberId));
