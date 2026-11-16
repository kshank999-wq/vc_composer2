import { collabRoute } from '@/lib/collab-route';
import { projectDetail } from '@/lib/collab-service';

export const runtime = 'nodejs';
export const dynamic = 'force-dynamic';

/** A shared project: the caller's role, the head, members, recent revisions, live locks. */
export const GET = (request: Request, { params }: { params: { id: string } }) =>
  collabRoute(request, (db, userId) => projectDetail(db, params.id, userId));
