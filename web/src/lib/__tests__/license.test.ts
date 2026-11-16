import { describe, expect, it } from 'vitest';
import { generateSerial, isWellFormedSerial } from '../license';

describe('license serials', () => {
  it('produces serials in the documented shape', () => {
    const serial = generateSerial();
    expect(serial).toMatch(/^VCC(-[A-Z2-9]{5}){4}$/);
    expect(isWellFormedSerial(serial)).toBe(true);
  });

  it('avoids characters that are misread over the phone', () => {
    const body = generateSerial().replaceAll('-', '').slice(3);
    for (const ambiguous of ['I', 'O', '0', '1']) {
      expect(body).not.toContain(ambiguous);
    }
  });

  it('does not repeat within a large sample', () => {
    const seen = new Set<string>();
    for (let i = 0; i < 2000; i += 1) seen.add(generateSerial());
    expect(seen.size).toBe(2000);
  });

  it('carries the VC Composer prefix and refuses another product\'s', () => {
    expect(generateSerial().startsWith('VCC-')).toBe(true);
    expect(isWellFormedSerial('VCW-ABCDE-FGHJK-LMNPQ-RSTUV')).toBe(false);
    expect(isWellFormedSerial('vcc-abcde-fghjk-lmnpq-rstuv')).toBe(true);
  });

  it('rejects malformed input', () => {
    expect(isWellFormedSerial('VCC-1234')).toBe(false);
    expect(isWellFormedSerial('not a serial')).toBe(false);
  });
});
