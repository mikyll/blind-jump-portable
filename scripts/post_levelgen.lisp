;;;
;;; This hook will be evaluated when the game finishes generating a new level.
;;;


;; Sometime, spawn a swarm of enemies after clearing a waypoint. The
;; waypoint_clear hook will look for the swarm variable in the environment.
(if (equal (cr-choice 4) 0)
    (if (and
         (> (level) 1)
         (not (equal (level) boss-0-level))
         (not (equal (level) boss-1-level))
         (not (equal (level) boss-2-level))
         (not (equal (level) boss-3-level)))
        (set 'swarm (if (and (> (level) boss-0-level)
                                  (equal (cr-choice 3) 0))
                        enemy-scarecrow
                        enemy-drone))))


;; The only way that we could end up in this scenario, is if we previously died
;; on a swarm level.
(if (and (equal (level) 0)
         (> swarm -1))
    (set 'swarm -1))


(map (lambda ($0)) post-levelgen-hooks)
