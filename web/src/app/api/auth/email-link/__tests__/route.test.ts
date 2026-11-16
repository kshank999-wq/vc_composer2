import { beforeEach, describe, expect, it, vi } from 'vitest';

/**
 * The emailed-link route, including with sign-ups switched off in Supabase,
 * where Supabase answers 422 `signup_disabled`, "Signups not allowed for this
 * instance".
 */
const signInWithOtp = vi.fn();
const resetPasswordForEmail = vi.fn();
const createUser = vi.fn();
const verify = vi.fn();

vi.mock('@/lib/supabase', () => ({
  serverClient: () => ({ auth: { signInWithOtp, resetPasswordForEmail } }),
  adminClient: () => ({ auth: { admin: { createUser } } }),
}));
vi.mock('@/lib/rate-limit', () => ({ RULES: { emailLink: {} }, clientAddress: () => '1.2.3.4', rateLimit: async () => null }));
vi.mock('@/lib/turnstile', () => ({ verifyTurnstile: (...args: unknown[]) => verify(...args) }));

const { POST } = await import('../route');
const ask = (body: unknown) =>
  POST(new Request('https://vc-composer.com/api/auth/email-link', { method: 'POST', body: JSON.stringify(body) }));
const off = { message: 'Signups not allowed for this instance', code: 'signup_disabled', status: 422 };

describe('POST /api/auth/email-link', () => {
  beforeEach(() => {
    vi.clearAllMocks();
    verify.mockResolvedValue({ ok: true, skipped: false });
  });

  it('refuses without the CAPTCHA, and sends nothing', async () => {
    verify.mockResolvedValue({ ok: false, reason: 'missing' });
    const response = await ask({ kind: 'link', email: 'a@b.co' });
    expect(response.status).toBe(400);
    expect(signInWithOtp).not.toHaveBeenCalled();
    expect(createUser).not.toHaveBeenCalled();
  });

  it('signs in an existing account with one request', async () => {
    signInWithOtp.mockResolvedValue({ error: null });
    expect((await ask({ kind: 'link', email: 'a@b.co', token: 't' })).status).toBe(200);
    expect(signInWithOtp).toHaveBeenCalledTimes(1);
    expect(createUser).not.toHaveBeenCalled();
  });

  it('makes a new person’s account itself when sign-ups are off, then sends the link', async () => {
    signInWithOtp.mockResolvedValueOnce({ error: off }).mockResolvedValueOnce({ error: null });
    createUser.mockResolvedValue({ error: null });
    expect((await ask({ kind: 'link', email: 'New@B.co', token: 't' })).status).toBe(200);
    expect(createUser).toHaveBeenCalledWith({ email: 'new@b.co' });
    expect(signInWithOtp.mock.calls[1]?.[0]?.options?.shouldCreateUser).toBe(false);
  });

  it('answers a reset the same whether or not the address has an account', async () => {
    resetPasswordForEmail.mockResolvedValue({ error: null });
    expect((await ask({ kind: 'reset', email: 'a@b.co', token: 't' })).status).toBe(200);
    expect(createUser).not.toHaveBeenCalled();
  });
});
