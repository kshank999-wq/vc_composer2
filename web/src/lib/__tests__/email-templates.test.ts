import { mkdirSync, writeFileSync } from 'node:fs';
import { describe, expect, it } from 'vitest';
import { licenseGranted, licenseReminder, purchaseConfirmation } from '../email-templates';

/**
 * What a customer receives is a contract: the serial, the link, and a version
 * number that changes whenever the words or the look do, because that number
 * is what `email_events` records.
 *
 * Set EMAIL_PREVIEW_DIR to also write the rendered HTML out, for opening in a
 * browser: the only honest way to check an email template.
 */

const inputs = {
  serial: 'VCC-7Q2M4-K9RT3-XAPLD-A8N6C',
  accountUrl: 'https://vc-composer.com/account',
};

describe('email templates', () => {
  it('put the serial and the download link in both the HTML and the text', () => {
    for (const email of [
      purchaseConfirmation({ ...inputs, platform: 'windows' }),
      purchaseConfirmation({ ...inputs, platform: null }),
      licenseReminder(inputs),
    ]) {
      expect(email.html).toContain(inputs.serial);
      expect(email.html).toContain(inputs.accountUrl);
      expect(email.text).toContain(inputs.serial);
      expect(email.text).toContain(inputs.accountUrl);
    }
  });

  it('speak as VC Composer 2 and never as another product', () => {
    for (const email of [purchaseConfirmation({ ...inputs, platform: 'macos' }), licenseReminder(inputs)]) {
      expect(email.subject).toContain('VC Composer 2');
      expect(email.html).toContain('VC COMPOSER');
      expect(`${email.html}${email.text}`).not.toMatch(/VC Writer|vc-writer/i);
    }
  });

  it('name the platform that was chosen', () => {
    expect(purchaseConfirmation({ ...inputs, platform: 'macos' }).html).toContain('macOS');
    expect(purchaseConfirmation({ ...inputs, platform: 'windows' }).text).toContain('Windows 10 / 11');
    expect(purchaseConfirmation({ ...inputs, platform: null }).text).toContain('Windows and macOS');
  });

  it('carry the version that email_events records', () => {
    expect(purchaseConfirmation({ ...inputs, platform: null })).toMatchObject({ template: 'purchase_confirmation', version: 1 });
    expect(licenseReminder(inputs)).toMatchObject({ template: 'license_reminder', version: 1 });
  });

  it('use only inline styles and no images, because email clients strip and block them', () => {
    const html = purchaseConfirmation({ ...inputs, platform: 'windows' }).html;
    expect(html).not.toContain('<style');
    expect(html).not.toContain('<link');
    expect(html).not.toContain('class=');
    expect(html).not.toContain('<img');
  });

  it('write previews when asked', () => {
    const dir = process.env['EMAIL_PREVIEW_DIR'];
    if (!dir) return;
    mkdirSync(dir, { recursive: true });
    writeFileSync(`${dir}/purchase-confirmation.html`, purchaseConfirmation({ ...inputs, platform: 'windows' }).html);
    writeFileSync(`${dir}/license-reminder.html`, licenseReminder(inputs).html);
  });
});

describe('a given licence', () => {
  it('says what it covers and how to start, and escapes who it is for', () => {
    const email = licenseGranted({
      serial: 'VCC-AAAAA-BBBBB-CCCCC-DDDDD',
      kind: 'academic',
      grantedTo: 'Hillside <High>',
      seats: '25 computers',
      expiresAt: '2027-06-30T23:59:59.000Z',
      accountUrl: 'https://vc-composer.com/account',
    });
    expect(email.subject).toBe('Your VC Composer 2 academic licence');
    expect(email.text).toContain('covers 25 computers until June 30, 2027');
    expect(email.html).toContain('Hillside &lt;High&gt;');
    expect(email.html).not.toContain('<High>');
    const master = licenseGranted({
      serial: 'X',
      kind: 'master',
      grantedTo: '',
      seats: 'any number of computers',
      expiresAt: null,
      accountUrl: 'u',
    });
    expect(master.text).toContain('covers any number of computers and does not expire');
    expect(master).toMatchObject({ template: 'license_granted', version: 1 });
  });
});
