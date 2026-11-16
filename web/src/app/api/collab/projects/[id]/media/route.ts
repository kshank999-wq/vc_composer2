import { mediaQuerySchema } from '@/domain';
import { badRequest, collabRoute } from '@/lib/collab-route';
import { mediaSlots } from '@/lib/collab-service';

export const runtime = 'nodejs';
export const dynamic = 'force-dynamic';

/** Which of these audio and picture files the project still needs, each with an upload link. */
export const POST = (request: Request, { params }: { params: { id: string } }) =>
  collabRoute(request, async (db, userId) => {
    const parsed = mediaQuerySchema.safeParse(await request.json().catch(() => null));
    if (!parsed.success) return badRequest('Name the files by their SHA-256 and size');
    return mediaSlots(db, params.id, userId, parsed.data.files);
  });
