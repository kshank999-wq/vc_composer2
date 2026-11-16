import { badRequest, collabRoute } from '@/lib/collab-route';
import { mediaLink } from '@/lib/collab-service';

export const runtime = 'nodejs';
export const dynamic = 'force-dynamic';

/** A short-lived link to download one of the project's media files. */
export const GET = (request: Request, { params }: { params: { id: string; sha: string } }) =>
  collabRoute(request, async (db, userId) => {
    if (!/^[0-9a-f]{64}$/.test(params.sha)) return badRequest('Name the file by its SHA-256');
    return mediaLink(db, params.id, userId, params.sha);
  });
