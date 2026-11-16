import { badRequest, collabRoute } from '@/lib/collab-route';
import { revisionLink } from '@/lib/collab-service';

export const runtime = 'nodejs';
export const dynamic = 'force-dynamic';

/** A short-lived link to download a revision ("head" for the latest is resolved by the client from the project). */
export const GET = (request: Request, { params }: { params: { id: string; number: string } }) =>
  collabRoute(request, async (db, userId) => {
    const n = Number.parseInt(params.number, 10);
    if (!Number.isFinite(n) || n < 1) return badRequest('Name a revision number');
    return revisionLink(db, params.id, userId, n);
  });
