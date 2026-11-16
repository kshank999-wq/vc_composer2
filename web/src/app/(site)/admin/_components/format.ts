/** Shared display helpers for the console's tables and tiles. */

export const formatDate = (value: string | null | undefined): string => {
  if (!value) return '—';
  const date = new Date(value);
  return Number.isNaN(date.getTime())
    ? '—'
    : date.toLocaleString('en-US', {
        year: 'numeric',
        month: 'short',
        day: 'numeric',
        hour: 'numeric',
        minute: '2-digit',
        timeZone: 'UTC',
        timeZoneName: 'short',
      });
};

export type PillTone = 'ok' | 'warn' | 'bad' | 'neutral';

/** Order status → how the pill reads. Refunds and disputes are what to look at. */
export const orderTone = (status: string): PillTone => {
  switch (status) {
    case 'paid':
      return 'ok';
    case 'pending':
      return 'warn';
    case 'refunded':
    case 'disputed':
    case 'failed':
      return 'bad';
    default:
      return 'neutral';
  }
};

export const licenseTone = (status: string | null): PillTone => {
  switch (status) {
    case 'active':
      return 'ok';
    case 'suspended':
      return 'warn';
    case 'revoked':
    case 'expired':
      return 'bad';
    default:
      return 'neutral';
  }
};

export const emailTone = (status: string): PillTone => {
  switch (status) {
    case 'sent':
    case 'delivered':
      return 'ok';
    case 'queued':
      return 'warn';
    case 'failed':
    case 'bounced':
      return 'bad';
    default:
      return 'neutral';
  }
};

/** The customer record, with the email pre-filled so it opens straight away. */
export const customerHref = (email: string | null): string =>
  email ? `/admin/support?email=${encodeURIComponent(email)}` : '/admin/support';
