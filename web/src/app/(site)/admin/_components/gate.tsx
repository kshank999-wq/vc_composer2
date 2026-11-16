import Link from 'next/link';

/** What a non-administrator sees on any console page. */
export function AdminGate({ title, next }: { title: string; next: string }) {
  return (
    <>
      <div className="hero">
        <h1>{title}</h1>
        <p>This area is for administrators.</p>
      </div>
      <Link href={`/signin?next=${encodeURIComponent(next)}`} className="button">
        Sign in
      </Link>
    </>
  );
}
