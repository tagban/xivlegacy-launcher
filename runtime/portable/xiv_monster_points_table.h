/* What the monster vendors sell (Monster Mode stage 5), in the order the shop window lists it: item
 * id and price in Monster Points. The server's vendor script must charge the same prices for the
 * same rows (the client only shows them). Ids and prices are XIVLegacy's own; nothing here comes
 * from the game's files. */
static const struct { uint32_t item, price; } MONSTER_SHOP[] = {
    {8013601, 1500}, /* Darklight Helm */
    {8013602, 1500}, /* Darklight Eyepatch */
    {8051502, 1500}, /* Darklight Breeches */
    {8071502, 1500}, /* Darklight Gauntlets */
    {8071503, 1500}, /* Darklight Bracers */
    {8071504, 1500}, /* Darklight Gloves */
    {8081901, 1500}, /* Darklight Sollerets */
    {8081902, 1500}, /* Darklight Caligae */
    {8081903, 1500}, /* Darklight Boots */
    {8032801, 2500}, /* Heavy Darklight Armor */
    {8032802, 2500}, /* Darklight Cuirass */
    {8032803, 2500}, /* Darklight Corselet */
    {8032804, 2500}, /* Darklight Cowl */
    {4020112, 5000}, /* Murderous Mogfists */
    {4030407, 5000}, /* Morbid Mogblade */
    {4040013, 5000}, /* Malignant Mogaxe */
    {4070214, 5000}, /* Mischievous Mogbow */
    {4080212, 5000}, /* Melancholy Mogfork */
    {5020111, 5000}, /* Maleficent Mogstaff */
    {5030036, 5000}, /* Malevolent Mogwand */
    {8013635, 800},  /* Dalamud Horn */
};
