//! The complete FFXIV 1.x patch chain, from the boot patch to 2012.09.19.0001 (1.23b).
//! Names sort into apply order. Sizes and SHA-1s were taken from the files.

pub struct Patch {
    pub name: &'static str,
    pub size: u64,
    pub sha1: &'static str,
}

pub const PATCHES: &[Patch] = &[
    Patch { name: "D2010.09.18.0000.patch", size: 5571687, sha1: "a693dd7c2104dcfbe34560845ee6647becb8c7f8" },
    Patch { name: "D2010.09.19.0000.patch", size: 444398866, sha1: "58afd2b4da95eab727551df4431973bc9ec12cff" },
    Patch { name: "D2010.09.23.0000.patch", size: 6907277, sha1: "d49fdb7588f1e3661c36c69af1f2a60bfa81fb7c" },
    Patch { name: "D2010.09.28.0000.patch", size: 18803280, sha1: "7ac14a8deddeb335c937684709b1e4dcb9ba159c" },
    Patch { name: "D2010.10.07.0001.patch", size: 19226330, sha1: "236d1a4cbffdb446a8618dbf57af1ae37518348b" },
    Patch { name: "D2010.10.14.0000.patch", size: 19464329, sha1: "33d1d98b690801791f79786449e9c80b7f79936f" },
    Patch { name: "D2010.10.22.0000.patch", size: 19778252, sha1: "c117509fe3897c37bfbc34933fb73a9646a13a8a" },
    Patch { name: "D2010.10.26.0000.patch", size: 19778391, sha1: "b9f36bb0183dfdfe0159dc88a590c11c72a51d7f" },
    Patch { name: "D2010.11.25.0002.patch", size: 250718651, sha1: "745a262a4466c1a977dc671e0d82dc7a0b0b7bd1" },
    Patch { name: "D2010.11.30.0000.patch", size: 6921623, sha1: "1428ed63fcface65bf72354b800f3c0af4318144" },
    Patch { name: "D2010.12.06.0000.patch", size: 7158904, sha1: "b13409ebe13bf69e1ae663c02cb338a276423212" },
    Patch { name: "D2010.12.13.0000.patch", size: 263311481, sha1: "efdbfe26b98c295b90ef14442f01dac644897e61" },
    Patch { name: "D2010.12.21.0000.patch", size: 7521358, sha1: "eb04744aec05fcbc56bdd31ab1cd79431754bfa9" },
    Patch { name: "D2011.01.18.0000.patch", size: 9954265, sha1: "7d3f37d20cbf0579e08aef9394dac1bc23b8da19" },
    Patch { name: "D2011.02.01.0000.patch", size: 11632816, sha1: "d12a56703be89809361d3d551c2e3a25cfae3a0f" },
    Patch { name: "D2011.02.10.0000.patch", size: 11714096, sha1: "c7000098ca9af6b53b6b9be2a4ddf587520962b9" },
    Patch { name: "D2011.03.01.0000.patch", size: 77464101, sha1: "c2b482d1c602d1c859440f7e4a66f006ffd584db" },
    Patch { name: "D2011.03.24.0000.patch", size: 108923937, sha1: "25a98d24880b30a81f8134fe9f7c3c1486e4c6f6" },
    Patch { name: "D2011.03.30.0000.patch", size: 109010880, sha1: "0fd0156718d49a326103b6c535514fa42bfa4a8c" },
    Patch { name: "D2011.04.13.0000.patch", size: 341603850, sha1: "16370e293c03d39919e7801ca4e37c769c016eb6" },
    Patch { name: "D2011.04.21.0000.patch", size: 343579198, sha1: "be31941a2290de27ac9f940f59b9e0ff04b99c83" },
    Patch { name: "D2011.05.19.0000.patch", size: 344239925, sha1: "baf99e56051bfb93b2fd765ce6bcad68a87c2676" },
    Patch { name: "D2011.06.10.0000.patch", size: 344334860, sha1: "502f6ed1f15c44bccf52049d7a459834aa7b6df6" },
    Patch { name: "D2011.07.20.0000.patch", size: 584926805, sha1: "1148f863782f84ebf3497fe14f3b4ae5928d6d82" },
    Patch { name: "D2011.07.26.0000.patch", size: 7649141, sha1: "9fd9208537929425b07fa37d42e7a7d2e33d5dc5" },
    Patch { name: "D2011.08.05.0000.patch", size: 152064532, sha1: "69203994a942ec8bc3876d623172f5e7cff1afa9" },
    Patch { name: "D2011.08.09.0000.patch", size: 8573687, sha1: "14d2801bfc3ce779a98d88f6da51320464fb4652" },
    Patch { name: "D2011.08.16.0000.patch", size: 6118907, sha1: "fa161991ba15ff0e6a52eec21196df1942d0335a" },
    Patch { name: "D2011.10.04.0000.patch", size: 677633296, sha1: "17d7f19ef41d6fcefa47278a784b62a81667605c" },
    Patch { name: "D2011.10.12.0001.patch", size: 28941655, sha1: "23e532833605b92aa1208cf097944162f7a0a983" },
    Patch { name: "D2011.10.27.0000.patch", size: 29179764, sha1: "db4641acfaca05d93767aff16cd30fea527d00c6" },
    Patch { name: "D2011.12.14.0000.patch", size: 374617428, sha1: "2adba232689d9e42973de19611ccf28e0d97b167" },
    Patch { name: "D2011.12.23.0000.patch", size: 22363713, sha1: "8315c146c3376da13ae36e5c09a52a10a7857ff6" },
    Patch { name: "D2012.01.18.0000.patch", size: 48998794, sha1: "668ee739dd99ebb7bd35cc38a593b4c3f0447170" },
    Patch { name: "D2012.01.24.0000.patch", size: 49126606, sha1: "46045d4f1beed1021b5be53c47a4e583b07fc02c" },
    Patch { name: "D2012.01.31.0000.patch", size: 49536396, sha1: "173466212d98652fa174844720b8fea257e8dd6b" },
    Patch { name: "D2012.03.07.0000.patch", size: 320630782, sha1: "fd648363bb079e90f8f153e3c9529c3ab4e8225b" },
    Patch { name: "D2012.03.09.0000.patch", size: 8312819, sha1: "8402bf2e087063ee7bc26e08861660950662bdbe" },
    Patch { name: "D2012.03.22.0000.patch", size: 22027738, sha1: "cd54a13345ad6d16a7d9f5813f8e799a5676045b" },
    Patch { name: "D2012.03.29.0000.patch", size: 8322920, sha1: "6ba54ebbab41068cc3516376e286fa28c71d0c0e" },
    Patch { name: "D2012.04.04.0000.patch", size: 8678570, sha1: "8a0ead99618c62c15ce213e1dbf41790be957fe9" },
    Patch { name: "D2012.04.23.0001.patch", size: 289511791, sha1: "87b302bf39b6fa4d1482f6eec3041f10efea6fec" },
    Patch { name: "D2012.05.08.0000.patch", size: 27266546, sha1: "b1904fd6d001a1c26c00be0773d38b67105ac3f3" },
    Patch { name: "D2012.05.15.0000.patch", size: 27416023, sha1: "f1e42bff04b6172727ce2fb1fdc92cec7cac6d06" },
    Patch { name: "D2012.05.22.0000.patch", size: 27742726, sha1: "c824fa615777fa8ab6353d9c0ca2aac4596d0ef9" },
    Patch { name: "D2012.06.06.0000.patch", size: 129984024, sha1: "6d0787761086c268de8f7d65d86bec490bef2759" },
    Patch { name: "D2012.06.19.0000.patch", size: 133434217, sha1: "d41e755c67542d63acb83aa3956d89bdae715f43" },
    Patch { name: "D2012.06.26.0000.patch", size: 133581048, sha1: "d3791b1cebe975312ab623198bbfdada1e0ea376" },
    Patch { name: "D2012.07.21.0000.patch", size: 253224781, sha1: "045018b53601d8e5060c27385e5acab471beaa27" },
    Patch { name: "D2012.08.10.0000.patch", size: 42851112, sha1: "a60c52f9659aa05e940b5ced0bae96ed3fbc5b95" },
    Patch { name: "D2012.09.06.0000.patch", size: 20566711, sha1: "9269b61e6859ff662b5608ea4235af4ca43f77d6" },
    Patch { name: "D2012.09.19.0001.patch", size: 20874726, sha1: "af006a8db729995194f7dbd0a2e3485be871a305" },
];

pub fn total_size() -> u64 {
    PATCHES.iter().map(|p| p.size).sum()
}

pub fn find(name: &str) -> Option<&'static Patch> {
    PATCHES.iter().find(|p| p.name.eq_ignore_ascii_case(name))
}
