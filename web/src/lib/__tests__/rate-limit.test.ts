import { describe, expect, it } from 'vitest';
import { RULES, clientAddress, rateLimit, rateLimitKey, type Consume } from '../rate-limit';

const request = (headers: Record<string, string> = {}): Request =>
  new Request('https://vc-composer.com/api/checkout', { method: 'POST', headers });

describe('rate limiting', () => {
  it('reads the client from the first hop of X-Forwarded-For', () => {
    expect(clientAddress(request({ 'x-forwarded-for': '203.0.113.9, 10.0.0.1' }))).toBe('203.0.113.9');
    expect(clientAddress(request({ 'x-real-ip': '203.0.113.10' }))).toBe('203.0.113.10');
    expect(clientAddress(request())).toBe('unknown');
  });

  it('keys by rule and address, never storing the address itself', () => {
    const a = rateLimitKey(RULES.checkout, request({ 'x-forwarded-for': '203.0.113.9' }));
    const b = rateLimitKey(RULES.checkout, request({ 'x-forwarded-for': '203.0.113.9' }));
    const other = rateLimitKey(RULES.checkout, request({ 'x-forwarded-for': '203.0.113.10' }));
    const otherRule = rateLimitKey(RULES.activate, request({ 'x-forwarded-for': '203.0.113.9' }));

    expect(a).toBe(b);
    expect(a).not.toBe(other);
    expect(a).not.toBe(otherRule);
    expect(a.startsWith('checkout:')).toBe(true);
    expect(a).not.toContain('203.0.113.9');
  });

  it('lets an allowed request through with nothing to send', async () => {
    const consume: Consume = async () => ({ allowed: true, retryAfterSeconds: 0 });
    expect(await rateLimit(request(), RULES.checkout, consume)).toBeNull();
  });

  it('answers 429 with Retry-After when the allowance is spent', async () => {
    const consume: Consume = async () => ({ allowed: false, retryAfterSeconds: 42 });
    const response = await rateLimit(request(), RULES.checkout, consume);

    expect(response?.status).toBe(429);
    expect(response?.headers.get('retry-after')).toBe('42');
    expect(((await response?.json()) as { error: string }).error).toMatch(/too many requests/i);
  });

  it('fails open when the limiter itself is broken', async () => {
    // The limiter guards against noise; it must never be the thing that
    // stops a customer paying.
    const consume: Consume = async () => {
      throw new Error('database unreachable');
    };
    expect(await rateLimit(request(), RULES.checkout, consume)).toBeNull();
  });

  it('hands the rule to the store unchanged', async () => {
    let seen: { key: string; limit: number; windowSeconds: number } | null = null;
    const consume: Consume = async (key, rule) => {
      seen = { key, limit: rule.limit, windowSeconds: rule.windowSeconds };
      return { allowed: true, retryAfterSeconds: 0 };
    };
    await rateLimit(request({ 'x-forwarded-for': '203.0.113.9' }), RULES.activate, consume);

    expect(seen).not.toBeNull();
    expect(seen!.limit).toBe(30);
    expect(seen!.windowSeconds).toBe(3600);
    expect(seen!.key.startsWith('activate:')).toBe(true);
  });
});
