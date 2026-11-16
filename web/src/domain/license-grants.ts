import { seatLimit, type License, type LicenseGrant } from './commerce';

/**
 * **Licences given rather than bought.**
 *
 * Two kinds, both issued by an administrator from the admin console:
 *
 * - **Academic**: a giveaway to a school, a class, a teacher or a student. It
 *   covers a set number of computers (two unless said otherwise, the same as
 *   one bought) and may end on a date, which is what a school year is.
 * - **Master**: the owner's own. It covers any number of computers and never
 *   ends, so every machine the owner composes on can be activated.
 *
 * Either is an ordinary licence row behind a $0 order, so activation,
 * downloads and the account page read it as they read any other.
 */

export type GivenLicense = Exclude<LicenseGrant, 'purchase'>;

/** What an academic licence covers unless the administrator says otherwise. */
export const ACADEMIC_SEATS = 2;
/** The most computers an academic licence may cover: a class's lab, not a campus. */
export const ACADEMIC_MAX_SEATS = 50;

/** One year on from `now`, at the end of that day: an academic licence's default term. */
export const academicDefaultEnd = (now: Date): string => {
  const end = new Date(Date.UTC(now.getUTCFullYear() + 1, now.getUTCMonth(), now.getUTCDate(), 23, 59, 59));
  return end.toISOString();
};

export interface LicenseGrantRequest {
  kind: GivenLicense;
  /** The account it goes to. An address with no account yet gets one. */
  email: string;
  /** Who or what it is for: the school and course, or the person. */
  grantedTo: string;
  /** Academic only: how many computers. Ignored on a master licence. */
  seats: number;
  /** Academic only: when it ends, or null for never. A master licence never ends. */
  expiresAt: string | null;
}

const EMAIL = /^[^\s@]+@[^\s@]+\.[^\s@]+$/;

/** Why this licence cannot be issued, in a sentence; null where it can. */
export const licenseGrantRefusal = (request: LicenseGrantRequest, now: Date): string | null => {
  if (!EMAIL.test(request.email.trim())) return 'Give the email address the licence is for.';
  if (request.kind === 'academic') {
    if (!request.grantedTo.trim()) return 'Say who an academic licence is for — the school and course, or the student.';
    if (!Number.isInteger(request.seats) || request.seats < 1 || request.seats > ACADEMIC_MAX_SEATS) {
      return `An academic licence covers from 1 to ${ACADEMIC_MAX_SEATS} computers.`;
    }
    if (request.expiresAt !== null) {
      const ends = new Date(request.expiresAt).getTime();
      if (Number.isNaN(ends)) return 'That end date cannot be read.';
      if (ends <= now.getTime()) return 'An academic licence has to end after today.';
    }
  }
  if (request.grantedTo.length > 200) return 'Keep who it is for under 200 characters.';
  return null;
};

/** The licence row's terms for a request: what the database is told. */
export const licenseGrantTerms = (
  request: LicenseGrantRequest,
): { grant: GivenLicense; maxActivations: number; expiresAt: string | null; grantedTo: string } =>
  request.kind === 'master'
    ? // The seat count keeps its ordinary default and is never read: `seatLimit`
      // answers "any number" for a master licence, so no large number stands in
      // for unlimited.
      { grant: 'master', maxActivations: ACADEMIC_SEATS, expiresAt: null, grantedTo: request.grantedTo.trim() }
    : {
        grant: 'academic',
        maxActivations: request.seats,
        expiresAt: request.expiresAt,
        grantedTo: request.grantedTo.trim(),
      };

/** What a licence covers, in a phrase: "2 computers", "any number of computers". */
export const licenseSeatsPhrase = (license: Pick<License, 'grant' | 'maxActivations'>): string => {
  const limit = seatLimit(license);
  if (limit === null) return 'any number of computers';
  return limit === 1 ? '1 computer' : `${limit} computers`;
};

/** A licence's kind, as its card calls it. */
export const licenseKindName = (grant: LicenseGrant): string =>
  grant === 'master' ? 'Master licence' : grant === 'academic' ? 'Academic licence' : 'Licence';
