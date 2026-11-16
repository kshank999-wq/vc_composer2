import { Resend } from 'resend';
import type { Platform } from '@/domain';
import { env } from './env';
import { adminClient } from './supabase';
import {
  collabInvite,
  type CollabInviteInput,
  licenseGranted,
  licenseReminder,
  purchaseConfirmation,
  type LicenseGrantedInput,
  type RenderedEmail,
} from './email-templates';

/**
 * Transactional email.
 *
 * Delivery outcomes are logged in `email_events` for support, with the
 * template and its version, so "which email did this customer actually get" is
 * answerable. Message bodies are not stored.
 *
 * A send failure must never fail the purchase: the licence already exists and
 * the customer can always retrieve it from My account, so the failure is
 * recorded and the caller carries on.
 */

interface SendResult {
  sent: boolean;
  error: string | null;
}

const deliver = async (input: { to: string; userId: string | null; email: RenderedEmail }): Promise<SendResult> => {
  const log = {
    user_id: input.userId,
    template: `${input.email.template}@${input.email.version}`,
    status: 'queued' as string,
    provider_message_id: null as string | null,
    error: null as string | null,
  };

  try {
    const resend = new Resend(env.resendApiKey);
    const { data, error } = await resend.emails.send({
      from: env.resendFrom,
      to: input.to,
      subject: input.email.subject,
      html: input.email.html,
      text: input.email.text,
    });
    if (error) throw new Error(error.message);
    log.status = 'sent';
    log.provider_message_id = data?.id ?? null;
  } catch (cause) {
    log.status = 'failed';
    log.error = cause instanceof Error ? cause.message : String(cause);
  }

  try {
    await adminClient().from('email_events').insert(log);
  } catch (cause) {
    // The log is for support; failing to write it must not fail the send.
    console.error('email_events', cause);
  }
  return { sent: log.status === 'sent', error: log.error };
};

const accountUrl = (): string => `${env.siteUrl}/account`;

export const sendPurchaseEmail = async (input: {
  to: string;
  userId: string;
  serial: string;
  platform: Platform | null;
}): Promise<SendResult> =>
  deliver({
    to: input.to,
    userId: input.userId,
    email: purchaseConfirmation({ serial: input.serial, platform: input.platform, accountUrl: accountUrl() }),
  });

export const sendLicenseReminder = async (input: { to: string; userId: string; serial: string }): Promise<SendResult> =>
  deliver({
    to: input.to,
    userId: input.userId,
    email: licenseReminder({ serial: input.serial, accountUrl: accountUrl() }),
  });

/** A licence an administrator has given. */
export const sendLicenseGranted = async (
  input: { to: string; userId: string } & Omit<LicenseGrantedInput, 'accountUrl'>,
): Promise<SendResult> =>
  deliver({
    to: input.to,
    userId: input.userId,
    email: licenseGranted({ ...input, accountUrl: accountUrl() }),
  });

/** An invitation to a shared project (collaboration). */
export const sendCollabInvite = async (input: { to: string } & Omit<CollabInviteInput, 'acceptUrl'> & { token: string }): Promise<SendResult> =>
  deliver({
    to: input.to,
    userId: null,
    email: collabInvite({ ...input, acceptUrl: `${env.siteUrl}/collaboration/invite/${input.token}` }),
  });
