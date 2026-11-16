import { collabRoute } from '@/lib/collab-route';
import { uploadSlot } from '@/lib/collab-service';

export const runtime = 'nodejs';
export const dynamic = 'force-dynamic';

/** Where to upload the next revision's file before pushing it. */
export const POST = (request: Request, { params }: { params: { id: string } }) =>
  collabRoute(request, (db, userId) => uploadSlot(db, params.id, userId));
