import { describe, expect, it } from 'vitest';
import {
  canLock,
  canManage,
  canPush,
  checkSeat,
  decideLock,
  decidePush,
  invitableRoles,
  pushSchema,
  isSafeMediaPath,
  mediaQuerySchema,
  MEDIA_MAX_BYTES,
  ROLES,
  seatsUsed,
  takesSeat,
} from '../index';

describe('collaboration roles', () => {
  it('has the five roles of the spec, the owner not invitable', () => {
    expect(ROLES).toEqual(['owner', 'editor', 'arranger', 'reviewer', 'viewer']);
    expect(invitableRoles).not.toContain('owner');
  });

  it('lets the people who write music push and lock; reviewers and viewers only read', () => {
    expect(ROLES.filter(canPush)).toEqual(['owner', 'editor', 'arranger']);
    expect(ROLES.filter(canLock)).toEqual(['owner', 'editor', 'arranger']);
    expect(ROLES.filter(canManage)).toEqual(['owner']);
  });

  it('charges a seat for editors and arrangers only', () => {
    expect(ROLES.filter(takesSeat)).toEqual(['editor', 'arranger']);
  });
});

describe('editor seats (the owner pays per editor seat)', () => {
  const base = { planActive: true, seats: 2, used: 2, alreadySeated: false };

  it('invites reviewers and viewers free, even with every seat in use or no plan', () => {
    expect(checkSeat({ ...base, role: 'reviewer' })).toEqual({ ok: true });
    expect(checkSeat({ ...base, role: 'viewer', planActive: false })).toEqual({ ok: true });
  });

  it('refuses an editor when every seat is in use, saying what to do', () => {
    const r = checkSeat({ ...base, role: 'editor' });
    expect(r.ok).toBe(false);
    if (!r.ok) expect(r.reason).toMatch(/All 2 editor seats are in use/);
  });

  it('lets someone already seated in another of the owner\'s projects in without a new seat', () => {
    expect(checkSeat({ ...base, role: 'arranger', alreadySeated: true })).toEqual({ ok: true });
  });

  it('needs an active plan for editors', () => {
    expect(checkSeat({ ...base, role: 'editor', used: 0, planActive: false }).ok).toBe(false);
  });

  it('counts each person once across projects, by account or by invited address', () => {
    expect(
      seatsUsed([
        { userId: 'a', email: 'a@x.com', role: 'editor' },
        { userId: 'a', email: 'a@x.com', role: 'arranger' },
        { userId: null, email: 'B@x.com', role: 'editor' },
        { userId: null, email: 'b@x.com', role: 'editor' },
        { userId: 'c', email: 'c@x.com', role: 'reviewer' },
      ]),
    ).toBe(2);
  });
});

describe('push and pull with merge', () => {
  it('accepts a push made from the head as the next revision', () => {
    expect(decidePush(7, 7)).toEqual({ accepted: true, revision: 8 });
  });

  it('refuses a stale push, naming the head to pull and merge first', () => {
    expect(decidePush(6, 7)).toEqual({ accepted: false, head: 7 });
  });

  it('only takes a revision file the client uploaded to its project path', () => {
    const ok = {
      base: 0,
      sizeBytes: 1000,
      sha256: 'a'.repeat(64),
      site: 0x1a2b,
      path: '3f1c2b44-1111-4222-8333-944455556666/5a6b7c8d-1111-4222-8333-944455556666.vcc2',
    };
    expect(pushSchema.safeParse(ok).success).toBe(true);
    expect(pushSchema.safeParse({ ...ok, path: '../other/x.vcc2' }).success).toBe(false);
    expect(pushSchema.safeParse({ ...ok, site: 0 }).success).toBe(false);
  });
});

describe('media files', () => {
  it('takes only paths that stay inside the project folder', () => {
    for (const ok of ['Audio Files/Vox take 1.wav', 'Picture/Reel 2 v3.mov', 'a.wav', 'Audio Files/..hidden.wav'])
      expect(isSafeMediaPath(ok)).toBe(true);
    for (const bad of ['', '/etc/passwd', '../x.wav', 'Audio Files/../../x.wav', 'C:/x.wav', 'c:x.wav', 'a\\b.wav',
      'Audio Files//x.wav', './x.wav', 'x.wav/', 'a\0b'])
      expect(isSafeMediaPath(bad)).toBe(false);
  });
  it('a push carries its media manifest, each path once', () => {
    const ok = {
      base: 1,
      sizeBytes: 1000,
      sha256: 'a'.repeat(64),
      site: 7,
      path: '3f1c2b44-1111-4222-8333-944455556666/5a6b7c8d-1111-4222-8333-944455556666.vcc2',
    };
    expect(pushSchema.parse(ok).media).toEqual([]);
    const m = { path: 'Audio Files/a.wav', sha256: 'b'.repeat(64), sizeBytes: 10 };
    expect(pushSchema.safeParse({ ...ok, media: [m] }).success).toBe(true);
    expect(pushSchema.safeParse({ ...ok, media: [m, m] }).success).toBe(false);
    expect(pushSchema.safeParse({ ...ok, media: [{ ...m, path: '../a.wav' }] }).success).toBe(false);
    expect(pushSchema.safeParse({ ...ok, media: [{ ...m, sizeBytes: MEDIA_MAX_BYTES + 1 }] }).success).toBe(false);
  });
  it('asks about at least one file and at most a thousand', () => {
    expect(mediaQuerySchema.safeParse({ files: [] }).success).toBe(false);
    expect(mediaQuerySchema.safeParse({ files: [{ sha256: 'c'.repeat(64), sizeBytes: 5 }] }).success).toBe(true);
  });
});

describe('track locks', () => {
  const now = new Date('2026-11-16T12:00:00Z');
  it('is free when nobody holds it, or it ran out, or the asker holds it', () => {
    expect(decideLock(null, 'u1', now)).toEqual({ ok: true });
    expect(decideLock({ userId: 'u2', name: 'Ben', expiresAt: '2026-11-16T11:59:00Z' }, 'u1', now)).toEqual({ ok: true });
    expect(decideLock({ userId: 'u1', name: 'Ann', expiresAt: '2026-11-16T13:00:00Z' }, 'u1', now)).toEqual({ ok: true });
  });
  it('says who holds it and until when', () => {
    expect(decideLock({ userId: 'u2', name: 'Ben', expiresAt: '2026-11-16T13:00:00Z' }, 'u1', now)).toEqual({
      ok: false,
      heldBy: 'Ben',
      until: '2026-11-16T13:00:00Z',
    });
  });
});
