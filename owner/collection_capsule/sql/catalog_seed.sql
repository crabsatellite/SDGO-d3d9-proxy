-- Seed only the native catalog's current permanent inventory and pending unit gifts.
-- Prior audit/history ledgers are deliberately not promoted to catalog membership.
INSERT sdgo_collection_capsule.CatalogUnlocks(AccountNumber,UnitID,PendingTransactionID)
SELECT u.AccountNumber,u.UnitNumber,0 FROM dbo.tbl_UserUnits u
WHERE u.TimeLimit=0 AND u.RunOut=0
  AND NOT EXISTS(SELECT 1 FROM sdgo_collection_capsule.CatalogUnlocks c
                 WHERE c.AccountNumber=u.AccountNumber AND c.UnitID=u.UnitNumber)
GROUP BY u.AccountNumber,u.UnitNumber;
INSERT sdgo_collection_capsule.CatalogUnlocks(AccountNumber,UnitID,PendingTransactionID)
SELECT b.AccountNumber,c.RefIndex,0 FROM dbo.tbl_UserDropItemBox b
JOIN dbo.tbl_ItemCategory c ON c.CatalogueID=b.ItemCategory
WHERE c.CatalogueID/100000=11 AND c.RefIndex>0 AND c.TimeLimit=0
  AND NOT EXISTS(SELECT 1 FROM sdgo_collection_capsule.CatalogUnlocks unlocked
                 WHERE unlocked.AccountNumber=b.AccountNumber AND unlocked.UnitID=c.RefIndex)
GROUP BY b.AccountNumber,c.RefIndex;
