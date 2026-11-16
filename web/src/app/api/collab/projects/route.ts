import { projectNameSchema } from '@/domain';
import { badRequest, collabRoute } from '@/lib/collab-route';
import { createProject, listProjects } from '@/lib/collab-service';

export const runtime = 'nodejs';
export const dynamic = 'force-dynamic';

/** The shared projects the caller is in. */
export const GET = (request: Request) =>
  collabRoute(request, async (db, userId) => ({ ok: true, value: { projects: await listProjects(db, userId) } }));

/** Share a project: the caller owns it; the first revision is pushed next. */
export const POST = (request: Request) =>
  collabRoute(request, async (db, userId) => {
    const body = (await request.json().catch(() => null)) as { name?: unknown } | null;
    const name = projectNameSchema.safeParse(body?.name);
    if (!name.success) return badRequest(name.error.issues[0]?.message ?? 'Name the project');
    return createProject(db, userId, name.data);
  });
