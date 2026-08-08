## The Passage

Leaving a level is no longer a cut. Commit to the exit hole and the wall opens onto a duct; you crawl through the inside of the wall while the next level renders live in the opening ahead. Then the floor gives out and you drop into it, standing up where you landed. One continuous camera, no fades, no loading screen. The game now opens the same way: falling out of the title into the first level.

## Quality of life

This build is a sweep of the details you stare at most.

- **Doors stay sharp.** The adaptive renderer now carves doors out of every resolution drop, so the EXIT sign reads as a word while you walk at it instead of shimmering into noise, and the handle holds its shape mid-stride.
- **The desk seam is gone.** The far desk sprite now matches its 3D pop-in in orientation, fog, size, and camera angle. Walking up to a desk no longer flips it, brightens it, or grows it at the swap line.
- **Imports are repeatable.** Billboard bakes derive their camera from the model itself, so the next imported object lands correct on the first bake.
