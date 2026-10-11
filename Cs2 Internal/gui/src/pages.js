export const groups = ['Combat', 'Skins', 'Players', 'Effects', 'World', 'View', 'Local', 'Movement'];
export const groupOrder = [0, 7, 2, 6, 1, 4, 3, 5];
export const pages = [
  [23, 0, 'Rage'], [24, 0, 'Anti Aim'],
  [25, 7, 'Assists'],
  [6, 2, 'ESP'], [9, 2, 'Chams'], [22, 2, 'Models'],
  [28, 6, 'Player'],
  [0, 1, 'Weapons'], [1, 1, 'Knives'], [2, 1, 'Gloves'], [3, 1, 'Agents'], [4, 1, 'Custom Agents'],
  [7, 4, 'Dropped items'], [20, 4, 'Throwables'], [12, 4, 'Sky'], [13, 4, 'Lighting'], [14, 4, 'Atmosphere'],
  [15, 4, 'Particles'], [17, 4, 'Physics'],
  [30, 3, 'Weapon Camo'], [10, 3, 'Particles'], [11, 3, 'Impact Effects'], [21, 3, 'Images'],
  [5, 5, 'Camera'], [16, 5, 'Crosshair'], [8, 5, 'Widgets'],
].map(([id, group, label]) => ({ id, group, label }));
