import styles from './shell.module.css';

// Present on every screen: nothing in Chronos involves real money.
export function PaperBanner() {
  return (
    <div className={styles.paperBanner} role="note" aria-label="Paper trading notice">
      <span className={styles.paperMark}>PAPER TRADING</span>
      <span className={styles.paperText}>
        Simulated orders only. No order ever reaches a broker or exchange, and no real money is involved.
      </span>
    </div>
  );
}
