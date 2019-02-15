    pub unsafe fn {fn_name}(&self, {params}) -> {return_param} {{
        let {return_param_name} = ((*self.ops).{fn_name})(self.ctx, {raw_params});
        {return_param_name}
    }}
